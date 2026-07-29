// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#include "Platform.h"

#include "DocsControllers.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <drogon/drogon.h>

using namespace drogon;

namespace wc {

namespace {

char const* kSwaggerHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <title>FieldRuntime Console API</title>
  <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css" />
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
  <script>
    window.onload = () => SwaggerUIBundle({ url: '/api/docs/openapi.yaml', dom_id: '#swagger-ui' });
  </script>
</body>
</html>)HTML";

std::string readFile(std::string const& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot open OpenAPI document: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!f.eof() && f.fail()) {
        throw std::runtime_error("cannot read OpenAPI document: " + path);
    }
    return ss.str();
}

} // namespace

void registerDocs(std::string openapiPath) {
    app().registerHandler("/api/docs",
        [](HttpRequestPtr const&, std::function<void(HttpResponsePtr const&)>&& cb) {
            auto r = HttpResponse::newHttpResponse();
            r->setContentTypeCode(CT_TEXT_HTML);
            r->setBody(kSwaggerHtml);
            cb(r);
        }, {Get});

    app().registerHandler("/api/docs/openapi.yaml",
        [openapiPath = std::move(openapiPath)](
            HttpRequestPtr const&,
            std::function<void(HttpResponsePtr const&)>&& cb) {
            try {
                auto r = HttpResponse::newHttpResponse();
                r->setContentTypeString("application/yaml");
                r->setBody(readFile(openapiPath));
                cb(r);
            } catch (std::exception const& error) {
                auto r = HttpResponse::newHttpResponse();
                r->setStatusCode(k500InternalServerError);
                r->setContentTypeCode(CT_TEXT_PLAIN);
                r->setBody(error.what());
                cb(r);
            }
        }, {Get});
}

} // namespace wc
