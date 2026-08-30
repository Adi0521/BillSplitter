#pragma once
#include <crow.h>
#include <crow/middlewares/cors.h>

// The application type, shared by main.cpp and every route module.
//
// crow::SimpleApp is crow::App<> — an app with no middleware — so asking it for
// get_middleware<crow::CORSHandler>() does not compile. Every route module
// takes BsApp& rather than crow::SimpleApp& so that adding a middleware here
// updates all of them at once.
using BsApp = crow::App<crow::CORSHandler>;
