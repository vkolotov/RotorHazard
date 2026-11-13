#include "usb_link.h"

#include "cJSON.h"

namespace relay {

namespace {

std::string typeOf(const cJSON* m)
{
    const cJSON* t = cJSON_GetObjectItemCaseSensitive(m, "type");
    return cJSON_IsString(t) ? t->valuestring : "";
}

std::string printed(cJSON* m)
{
    char* text = cJSON_PrintUnformatted(m);
    std::string out = text ? text : "";
    cJSON_free(text);
    return out;
}

}  // namespace

void UsbLink::note(const std::string& line)
{
    if (log)
        log(line);
}

std::string UsbLink::info(bool wifiConnected) const
{
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "link_info");
    cJSON_AddStringToObject(m, "uid", timer_.uid.c_str());
    cJSON_AddStringToObject(m, "name", timer_.name.c_str());
    cJSON_AddStringToObject(m, "rh_version", rhVersion.c_str());
    cJSON_AddStringToObject(m, "plugin_version", PLUGIN_PROTOCOL_VERSION);
    // The relay is connected to mach1 by itself (its Wi-Fi socket).
    cJSON_AddBoolToObject(m, "cloud", wifiConnected && !via_);
    cJSON_AddBoolToObject(m, "via_link", via_);
    cJSON_AddStringToObject(m, "relay_uid", timer_.relayUid.c_str());
    const std::string out = printed(m);
    cJSON_Delete(m);
    return out;
}

std::string UsbLink::wrap(const std::string& mach1Json)
{
    return "{\"type\":\"link_send\",\"message\":" + mach1Json + "}";
}

void UsbLink::received(const char* line, int64_t now, bool wifiConnected)
{
    cJSON* m = cJSON_Parse(line);
    if (!cJSON_IsObject(m)) {
        cJSON_Delete(m);
        return;
    }
    const std::string type = typeOf(m);
    if (type.rfind("link_", 0) != 0) {
        cJSON_Delete(m);
        return;
    }
    if (type == "link_probe") {
        // Which kind of board is on this port: answered, nothing more.
        if (toHost)
            toHost(info(wifiConnected));
        cJSON_Delete(m);
        return;
    }
    seen_ = now;
    if (!alive_) {
        alive_ = true;
        note("USB: mission control is on the USB port");
    }
    if (type == "link_open") {
        const cJSON* t = cJSON_GetObjectItemCaseSensitive(m, "time_ms");
        if (cJSON_IsNumber(t) && t->valuedouble > 1.7e12 && hostClock)
            hostClock((int64_t)t->valuedouble);
        if (toHost)
            toHost(info(wifiConnected));
    } else if (type == "link_up") {
        up_ = true;
        prefer_ = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "prefer"));
        // A new socket to mach1 on the page: it gets what a fresh connection gets.
        update(now, wifiConnected, true);
    } else if (type == "link_cloud") {
        cJSON* message = cJSON_GetObjectItemCaseSensitive(m, "message");
        if (via_ && cJSON_IsObject(message))
            timer_.message(printed(message).c_str(), now);
    } else if (type == "link_down") {
        up_ = false;
        update(now, wifiConnected, false);
    }
    cJSON_Delete(m);
}

void UsbLink::tick(int64_t now, bool usbConnected, bool wifiConnected)
{
    if (alive_ && (!usbConnected || now - seen_ > USB_LINK_TIMEOUT_MS)) {
        alive_ = false;
        up_ = false;
        note(usbConnected ? "USB: mission control went quiet" : "USB: unplugged");
    }
    // Without `prefer`, the link only stands in while Wi-Fi is down.
    update(now, wifiConnected, false);
}

void UsbLink::drop(int64_t now, const std::string& why, bool askReconnect)
{
    if (!up_ && !via_)
        return;
    note("USB: " + why);
    up_ = false;
    if (askReconnect && toHost)
        toHost("{\"type\":\"link_reconnect\"}");
    // Back to Wi-Fi now; a reconnected page sends link_up and takes over again.
    update(now, false, false);
}

void UsbLink::identityChanged(bool wifiConnected)
{
    if (alive_ && toHost)
        toHost(info(wifiConnected));
}

void UsbLink::update(int64_t now, bool wifiConnected, bool fresh)
{
    const bool want = alive_ && up_ && (prefer_ || !wifiConnected);
    if (want != via_) {
        via_ = want;
        timer_.socketClosed();
        timer_.watchFrameMs = via_ ? USB_WATCH_FRAME_MS : WATCH_FRAME_MS;
        timer_.route = via_ ? "relay_usb" : "relay";
        note(via_ ? "mach1 through USB (mission control on the laptop)" : "mach1 through Wi-Fi again");
        if (routeChanged)
            routeChanged(via_);
        if (via_)
            timer_.socketOpen(now);
    } else if (fresh && via_) {
        timer_.socketOpen(now);
    }
}

}  // namespace relay
