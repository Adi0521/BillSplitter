#include "services/receipt/ocr.h"

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace receipt {
namespace {

// ── Preprocessing limits ─────────────────────────────────────────────────────
//
// The upload itself is capped at 10 MB, but compressed formats decode to far
// more than they weigh: a 2 MB PNG of a single flat colour can expand to a
// gigapixel buffer. The pixel budget is the real memory bound, so it is
// enforced on the decoded image, not on the file.
constexpr long long kMaxPixels = 60LL * 1000 * 1000;   // 60 MP
constexpr int       kMinWidth  = 1000;                 // upscale below this
constexpr int       kTargetWidth = 1800;               // upscale target
constexpr int       kMaxWidth  = 3500;                 // downscale above this
constexpr float     kMaxUpscale = 4.0f;

// Tesseract's LSTM engine is trained at roughly 300 dpi. Leaving the
// resolution unset makes it print "Invalid resolution 0 dpi" and guess, so it
// is stated explicitly after scaling.
constexpr int kAssumedDpi = 300;

int env_int(const char* name, int fallback, int lo, int hi) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    char* end = nullptr;
    const long v = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0' || v < lo || v > hi) return fallback;
    return static_cast<int>(v);
}

std::string env_str(const char* name) {
    const char* raw = std::getenv(name);
    return raw ? std::string(raw) : std::string();
}

// RAII for leptonica's PIX, which is malloc'd and freed through a C API that
// takes the address of the pointer.
class PixPtr {
public:
    PixPtr() = default;
    explicit PixPtr(PIX* p) : p_(p) {}
    ~PixPtr() { reset(nullptr); }

    PixPtr(const PixPtr&)            = delete;
    PixPtr& operator=(const PixPtr&) = delete;
    PixPtr(PixPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    PixPtr& operator=(PixPtr&& o) noexcept {
        if (this != &o) { reset(o.p_); o.p_ = nullptr; }
        return *this;
    }

    PIX* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void reset(PIX* p) {
        if (p_) pixDestroy(&p_);
        p_ = p;
    }

private:
    PIX* p_ = nullptr;
};

// ── Preprocessing ────────────────────────────────────────────────────────────
//
// This is where receipt OCR quality actually comes from. A phone photo of
// thermal paper is low contrast, unevenly lit, slightly rotated and often too
// small for the recognizer; feeding it in raw produces garbage that no line
// parser can rescue. Each step below targets one of those specific failures,
// in the order that makes the next step's job easier:
//
//   1. 8-bit grayscale (pixConvertTo8) — drops colour and any palette. Thermal
//      and dot-matrix receipts carry no colour information, and every operation
//      after this is defined on gray.
//   2. Background normalization (pixBackgroundNormSimple) — flattens the
//      lighting gradient and shadow from a hand-held photo, so that one global
//      idea of "white paper" holds across the whole image. Without it, the
//      binarizer clips a shadowed corner to solid black.
//   3. Deskew (pixDeskew) — Tesseract's line finder assumes near-horizontal
//      text; a couple of degrees of rotation from a hand-held shot merges
//      adjacent lines and destroys the name/price column split that the parser
//      depends on. Done on gray so the rotation interpolates instead of
//      chewing up already-binarized strokes.
//   4. Scale — up when the receipt is too small to have enough pixels per
//      character (the single biggest quality lever on phone photos), down when
//      it is a 12 MP original, which costs seconds of recognition for no gain.
//   5. Adaptive Otsu (pixOtsuAdaptiveThreshold, 300 px tiles) — per-tile
//      thresholds handle whatever contrast variation survived step 2, which a
//      single global threshold cannot. Faded thermal print is exactly the case
//      a global threshold erases.
//   6. Declare 300 dpi, matching what the LSTM model was trained on.
//
// Every step is a no-op-safe fallback: if a leptonica call returns null the
// previous image is kept rather than failing the request, since a slightly
// worse image still OCRs.
PixPtr preprocess(PIX* src) {
    PixPtr gray(pixConvertTo8(src, 0));
    if (!gray) throw ImageDecodeError("The image could not be converted to grayscale");

    if (PIX* normed = pixBackgroundNormSimple(gray.get(), nullptr, nullptr)) {
        gray.reset(normed);
    }

    // redsearch=4: the standard speed/accuracy tradeoff from leptonica's own
    // examples. Returns a clone when no confident skew angle is found.
    if (PIX* deskewed = pixDeskew(gray.get(), 4)) {
        gray.reset(deskewed);
    }

    const int w = pixGetWidth(gray.get());
    if (w > 0) {
        float factor = 1.0f;
        if (w < kMinWidth) {
            factor = std::min(kMaxUpscale,
                              static_cast<float>(kTargetWidth) / static_cast<float>(w));
        } else if (w > kMaxWidth) {
            factor = static_cast<float>(kMaxWidth) / static_cast<float>(w);
        }
        if (factor < 0.999f || factor > 1.001f) {
            if (PIX* scaled = pixScale(gray.get(), factor, factor)) {
                gray.reset(scaled);
            }
        }
    }

    PIX* binary = nullptr;
    if (pixOtsuAdaptiveThreshold(gray.get(), 300, 300, 0, 0, 0.1f, nullptr, &binary) == 0
        && binary != nullptr) {
        gray.reset(binary);
    }

    pixSetResolution(gray.get(), kAssumedDpi, kAssumedDpi);
    return gray;
}

std::string trim(const std::string& s) {
    const char* ws = " \t\n\r\f\v";
    const auto begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return "";
    const auto end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

}  // namespace

// ── TesseractPool ────────────────────────────────────────────────────────────

struct TesseractPool::Impl {
    std::string language;
    std::string tessdata;
    std::queue<tesseract::TessBaseAPI*> free_slots;   // null = uninitialized slot
    std::vector<tesseract::TessBaseAPI*> owned;       // everything ever created
    std::mutex mutex;
    std::condition_variable cv;
};

TesseractPool::TesseractPool(int pool_size, std::string language, std::string tessdata_dir)
    : impl_(new Impl()) {
    if (pool_size < 1) pool_size = 1;
    impl_->language = std::move(language);
    impl_->tessdata = std::move(tessdata_dir);
    for (int i = 0; i < pool_size; ++i) {
        impl_->free_slots.push(nullptr);   // empty slot, filled on first use
    }
    // Leptonica logs decode failures to stderr at WARNING severity. A corrupt
    // upload is an expected, handled 400, not something worth a wall of server
    // log, so only genuine errors are printed.
    setMsgSeverity(L_SEVERITY_ERROR);
}

TesseractPool::~TesseractPool() {
    for (auto* api : impl_->owned) {
        if (api) {
            api->End();
            delete api;
        }
    }
}

TesseractPool::Lease::~Lease() {
    pool_.release(api_);
}

void TesseractPool::release(tesseract::TessBaseAPI* api) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->free_slots.push(api);   // may be null: an empty slot
    }
    impl_->cv.notify_one();
}

TesseractPool::Lease TesseractPool::acquire() {
    tesseract::TessBaseAPI* api = nullptr;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->cv.wait(lock, [this] { return !impl_->free_slots.empty(); });
        api = impl_->free_slots.front();
        impl_->free_slots.pop();
    }

    if (!api) {
        auto* fresh = new tesseract::TessBaseAPI();
        const char* datapath = impl_->tessdata.empty() ? nullptr : impl_->tessdata.c_str();
        // OEM_DEFAULT rather than OEM_LSTM_ONLY: it prefers the LSTM engine
        // when the traineddata carries one (every current distribution does)
        // but still starts against a legacy-only tessdata instead of refusing.
        const int rc = fresh->Init(datapath, impl_->language.c_str(),
                                   tesseract::OEM_DEFAULT);
        if (rc != 0) {
            delete fresh;
            release(nullptr);   // give the slot back before propagating
            throw OcrUnavailable(
                "The OCR engine could not be initialized. Is the '" + impl_->language +
                "' tessdata installed?");
        }
        // Receipts are a single column of text whose columns are made of
        // spaces, so gap information must survive into the line text the
        // parser sees; by default tesseract collapses runs of spaces.
        fresh->SetVariable("preserve_interword_spaces", "1");
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->owned.push_back(fresh);
        }
        api = fresh;
    }
    return Lease(*this, api);
}

TesseractPool& TesseractPool::shared() {
    // Four engines against Crow's eight workers: OCR is CPU-bound, so more
    // concurrent recognitions than cores buys nothing and each LSTM instance
    // costs tens of MB. The ninth uploader waits instead of allocating.
    //
    // Deliberately leaked. libtesseract keeps process-global caches of the
    // loaded language model, and those are torn down by the library's own
    // static destructors; running TessBaseAPI::End() from a static destructor
    // of ours races that teardown and aborts the process on exit (observed:
    // "mutex lock failed" out of ObjectCache). Nothing is gained by freeing
    // engines microseconds before the kernel reclaims the address space, so
    // the singleton simply outlives main().
    static TesseractPool* pool = new TesseractPool(
        env_int("BILLSPLITTER_OCR_POOL", 4, 1, 32),
        "eng",
        !env_str("BILLSPLITTER_TESSDATA").empty() ? env_str("BILLSPLITTER_TESSDATA")
                                                  : env_str("TESSDATA_PREFIX"));
    return *pool;
}

// ── Recognition ──────────────────────────────────────────────────────────────

std::vector<OcrLine> recognize_image(const std::string& bytes) {
    if (bytes.empty()) throw ImageDecodeError("The uploaded file is empty");

    PixPtr src(pixReadMem(reinterpret_cast<const l_uint8*>(bytes.data()), bytes.size()));
    if (!src) {
        throw ImageDecodeError(
            "The image could not be decoded. Upload a PNG, JPEG, WebP or TIFF file.");
    }

    const long long pixels =
        static_cast<long long>(pixGetWidth(src.get())) * pixGetHeight(src.get());
    if (pixels <= 0) {
        throw ImageDecodeError("The image has no pixels");
    }
    if (pixels > kMaxPixels) {
        throw ImageDecodeError("The image is too large to process. Please upload a smaller photo.");
    }

    PixPtr prepared = preprocess(src.get());

    std::vector<OcrLine> lines;
    {
        auto lease = TesseractPool::shared().acquire();
        tesseract::TessBaseAPI& api = lease.get();

        api.SetImage(prepared.get());
        api.SetSourceResolution(kAssumedDpi);
        // A receipt is one uniform block of text, not a magazine page. AUTO
        // hunts for columns and regularly splits a receipt's name and price
        // columns into two "regions", which would shuffle prices away from the
        // lines they belong to.
        api.SetPageSegMode(tesseract::PSM_SINGLE_BLOCK);

        if (api.Recognize(nullptr) != 0) {
            api.Clear();
            throw OcrUnavailable("The OCR engine failed while reading this image");
        }

        std::unique_ptr<tesseract::ResultIterator> it(api.GetIterator());
        if (it) {
            do {
                char* raw = it->GetUTF8Text(tesseract::RIL_TEXTLINE);
                if (!raw) continue;
                std::string text = trim(std::string(raw));
                delete[] raw;
                if (text.empty()) continue;

                // Confidence(RIL_TEXTLINE) is the mean of the word confidences
                // on the line, on tesseract's 0–100 scale. It is a statement
                // about legibility only: a crisply printed wrong price scores
                // 1.0, which is why the contract calls it a hint.
                float conf = it->Confidence(tesseract::RIL_TEXTLINE);
                if (conf < 0.0f) conf = 0.0f;
                if (conf > 100.0f) conf = 100.0f;

                lines.push_back(OcrLine{std::move(text), conf / 100.0});
            } while (it->Next(tesseract::RIL_TEXTLINE));
        }
        api.Clear();   // drop the image before the engine goes back in the pool
    }
    return lines;
}

// ── Text and HTML ────────────────────────────────────────────────────────────

std::vector<std::string> split_text_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            std::string trimmed = trim(current);
            if (!trimmed.empty()) lines.push_back(std::move(trimmed));
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    std::string trimmed = trim(current);
    if (!trimmed.empty()) lines.push_back(std::move(trimmed));
    return lines;
}

namespace {

bool starts_with_ci(const std::string& hay, std::size_t pos, const char* needle) {
    const std::size_t n = std::strlen(needle);
    if (pos + n > hay.size()) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(hay[pos + i])) !=
            std::tolower(static_cast<unsigned char>(needle[i]))) {
            return false;
        }
    }
    return true;
}

void append_utf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Only the entities that actually appear in a receipt export: currency and
// punctuation, plus numeric references. An unknown entity is left verbatim
// rather than dropped, so a price never loses a character silently.
void decode_entity(const std::string& name, std::string& out) {
    static const struct { const char* name; const char* utf8; } kNamed[] = {
        {"amp", "&"},   {"lt", "<"},     {"gt", ">"},    {"quot", "\""},
        {"apos", "'"},  {"nbsp", " "},   {"ndash", "-"}, {"mdash", "-"},
        {"dollar", "$"},{"cent", "\xC2\xA2"}, {"pound", "\xC2\xA3"},
        {"euro", "\xE2\x82\xAC"}, {"yen", "\xC2\xA5"},
    };
    for (const auto& e : kNamed) {
        if (name == e.name) { out += e.utf8; return; }
    }
    if (name.size() > 1 && name[0] == '#') {
        const bool hex = (name[1] == 'x' || name[1] == 'X');
        const std::string digits = name.substr(hex ? 2 : 1);
        if (!digits.empty()) {
            char* end = nullptr;
            const unsigned long cp = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
            if (end && *end == '\0' && cp > 0) {
                append_utf8(out, cp);
                return;
            }
        }
    }
    out += "&" + name + ";";
}

// Tags whose end (or self-close) starts a new visual line. Everything else is
// inline and must not break a "NAME .... 3.49" row into two lines.
bool is_block_tag(const std::string& tag) {
    static const char* kBlocks[] = {
        "tr", "p", "div", "li", "br", "table", "tbody", "thead", "h1", "h2",
        "h3", "h4", "h5", "h6", "section", "article", "header", "footer",
        "ul", "ol", "dl", "dt", "dd", "hr", "form", "pre", "blockquote"};
    for (const char* b : kBlocks) {
        if (tag == b) return true;
    }
    return false;
}

}  // namespace

std::string html_to_text(const std::string& html) {
    std::string out;
    out.reserve(html.size() / 2);

    std::size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            // <script> and <style> bodies are code, not receipt text, and
            // <title> is head metadata rather than anything the user saw on
            // the page. Skipping to the matching close tag keeps a pile of
            // JavaScript out of the parser's unmatched_lines, and keeps the
            // browser tab caption ("Your order") from being mistaken for the
            // store name that the page's own heading actually carries.
            for (const char* raw : {"script", "style", "title"}) {
                const std::string open = std::string("<") + raw;
                if (starts_with_ci(html, i, open.c_str())) {
                    const std::string close = std::string("</") + raw;
                    std::size_t end = i + open.size();
                    while (end < html.size() && !starts_with_ci(html, end, close.c_str())) {
                        ++end;
                    }
                    if (end < html.size()) {
                        end = html.find('>', end);
                        i = (end == std::string::npos) ? html.size() : end + 1;
                    } else {
                        i = html.size();
                    }
                    break;
                }
            }
            if (i >= html.size()) break;
            if (html[i] != '<') continue;   // a script/style block was skipped

            // <!-- comment -->
            if (starts_with_ci(html, i, "<!--")) {
                const std::size_t end = html.find("-->", i + 4);
                i = (end == std::string::npos) ? html.size() : end + 3;
                continue;
            }

            const std::size_t close = html.find('>', i);
            if (close == std::string::npos) break;   // unterminated tag: drop the rest

            std::string name;
            std::size_t p = i + 1;
            if (p < close && html[p] == '/') ++p;
            while (p < close && (std::isalnum(static_cast<unsigned char>(html[p])) ||
                                 html[p] == '!')) {
                name.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(html[p]))));
                ++p;
            }
            // A cell boundary is a column boundary: two spaces, so the line
            // parser sees the same "name<gap>price" shape it sees from OCR.
            if (name == "td" || name == "th") out += "  ";
            if (is_block_tag(name)) out.push_back('\n');
            i = close + 1;
            continue;
        }

        if (html[i] == '&') {
            const std::size_t semi = html.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 10) {
                decode_entity(html.substr(i + 1, semi - i - 1), out);
                i = semi + 1;
                continue;
            }
        }

        // Collapse runs of horizontal whitespace to at most two spaces: enough
        // to preserve a column gap, not enough to leave a 40-space indent.
        if (html[i] == ' ' || html[i] == '\t') {
            std::size_t run = 0;
            while (i < html.size() && (html[i] == ' ' || html[i] == '\t')) { ++i; ++run; }
            out.append(run >= 2 ? 2 : 1, ' ');
            continue;
        }
        out.push_back(html[i]);
        ++i;
    }
    return out;
}

}  // namespace receipt
