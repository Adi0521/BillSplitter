#pragma once
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Local OCR for receipt images.
//
// Tesseract is used in-process rather than through a cloud API, so a user's
// purchase history never leaves this host and no API key is required. Two
// facts about the library shape everything below:
//
//   1. tesseract::TessBaseAPI is NOT thread-safe. Crow serves requests on 8
//      worker threads, so a single shared instance would corrupt recognition
//      state under concurrent uploads.
//   2. Init() is expensive (tens of milliseconds and tens of MB for the LSTM
//      model), so constructing one per request is not an option either.
//
// Hence TesseractPool: a small fixed set of initialized engines handed out
// under an RAII lease, exactly like DbPool hands out connections.

namespace tesseract {
class TessBaseAPI;
}

namespace receipt {

// One recognized text line and how legible the engine found it.
struct OcrLine {
    std::string text;
    double      confidence;  // mean word confidence for the line, 0.0–1.0
};

// The image could not be decoded, is not a format leptonica understands, or is
// absurdly large. This is caller error: a 400, never a 500.
class ImageDecodeError : public std::runtime_error {
public:
    explicit ImageDecodeError(const std::string& what) : std::runtime_error(what) {}
};

// The engine itself is unavailable — no tessdata, failed Init, recognition
// aborted. This is a server fault: a 500.
class OcrUnavailable : public std::runtime_error {
public:
    explicit OcrUnavailable(const std::string& what) : std::runtime_error(what) {}
};

// A fixed-size pool of initialized TessBaseAPI instances.
//
// Engines are created lazily: the pool starts with `pool_size` empty slots and
// initializes a real engine the first time a slot is used, so the server still
// starts when tessdata is missing — the failure surfaces on the request that
// needs OCR rather than at process launch. acquire() blocks when every engine
// is busy, which is also what bounds the memory an upload storm can consume.
class TesseractPool {
public:
    // language: tesseract language code ("eng"). tessdata_dir may be empty, in
    // which case TESSDATA_PREFIX and the compiled-in default are used.
    TesseractPool(int pool_size, std::string language, std::string tessdata_dir);
    ~TesseractPool();

    TesseractPool(const TesseractPool&)            = delete;
    TesseractPool& operator=(const TesseractPool&) = delete;

    // RAII lease: holds one engine and returns it to the pool on destruction.
    //
    // Non-copyable AND non-movable, for the same reason DbPool::Guard is:
    // C++17 guaranteed copy elision makes a move constructor unnecessary for
    // `return Lease(...)`, and having one would allow a moved-from lease to
    // return a null engine to the pool alongside the real one.
    class Lease {
    public:
        Lease(TesseractPool& pool, tesseract::TessBaseAPI* api)
            : pool_(pool), api_(api) {}
        ~Lease();

        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&)                 = delete;
        Lease& operator=(Lease&&)      = delete;

        tesseract::TessBaseAPI& get() const { return *api_; }
        tesseract::TessBaseAPI* operator->() const { return api_; }

    private:
        TesseractPool&          pool_;
        tesseract::TessBaseAPI* api_;
    };

    // Blocks until an engine is free. Throws OcrUnavailable if the engine
    // cannot be initialized; the slot is returned to the pool either way.
    Lease acquire();

    // The process-wide pool. Sized from BILLSPLITTER_OCR_POOL (default 4) and
    // pointed at BILLSPLITTER_TESSDATA / TESSDATA_PREFIX when set.
    static TesseractPool& shared();

private:
    friend class Lease;
    void release(tesseract::TessBaseAPI* api);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Decodes, preprocesses and OCRs an image held in memory, returning one entry
// per recognized text line in reading order.
//
// Throws ImageDecodeError for input that is not a decodable image (400) and
// OcrUnavailable when the engine itself fails (500).
std::vector<OcrLine> recognize_image(const std::string& bytes);

// Splits already-textual input (an HTML receipt, say) into the same line shape,
// with confidence 0 — nothing was guessed, so there is nothing to be unsure
// about. Blank lines are dropped and interior whitespace runs are preserved,
// since column gaps are what the line parser uses to find prices.
std::vector<std::string> split_text_lines(const std::string& text);

// Strips tags, script/style bodies and entities out of an HTML document,
// yielding the visible text one line per block element. HTML receipts are
// already text; running them through OCR would only add errors.
std::string html_to_text(const std::string& html);

}  // namespace receipt
