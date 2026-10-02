// src/web/WebServices.cpp: see WebServices.h. THE BASE COMMIT'S STUB (web Sprint C): it refuses every request, so that
// WebHost (card G-D) builds and links from its first minute. Card G-E replaces this file with the DOM menu and the
// clipboard.
#include "WebServices.h"

namespace funkgui::web
{
    struct WebServices::Impl {};

    WebServices::WebServices(const char*) : impl_(std::make_unique<Impl>()) {}
    WebServices::~WebServices() { letGo(); }

    bool WebServices::showMenu(const MenuRequest&, MenuCallback, float) { return false; }
    void WebServices::dismissMenus() {}
    bool WebServices::copyText(std::string_view) { return false; }
    void WebServices::letGo() {}
    bool WebServices::menuOpen() const noexcept { return false; }
}
