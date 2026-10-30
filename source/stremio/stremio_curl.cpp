// stremio_curl.cpp - libcurl-backed HttpPostFn.
//
// TLS: certificate verification is ON. The login request carries the user's
// password, so we refuse to send it without a CA bundle. Put cacert.pem
// (https://curl.se/docs/caextract.html) in romfs/ so "romfs:/cacert.pem" exists,
// or point makeCurlPost() at wherever your app already keeps one.
// Only define STREMIO_INSECURE_TLS for local debugging.
//
// Assumes the app already called curl_global_init() (SwitchStream calls it in main.cpp).
#include <curl/curl.h>

#include <cstdio>
#include <string>

#include "stremio_account.h"

namespace stremio {

static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

static int xferInfoCb(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return cancelRequested() ? 1 : 0;  // non-zero aborts the transfer
}

static bool fileExists(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

HttpPostFn makeCurlPost(const std::string& caBundlePath) {
    return [caBundlePath](const std::string& url, const std::string& body, std::string& response,
                          std::string& err) -> bool {
#ifndef STREMIO_INSECURE_TLS
        if (!fileExists(caBundlePath)) {
            err = "CA bundle not found (" + caBundlePath + ")";
            return false;
        }
#endif
        CURL* c = curl_easy_init();
        if (!c) { err = "curl init failed"; return false; }

        response.clear();
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");

        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferInfoCb);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "SwitchStream");
#ifdef STREMIO_INSECURE_TLS
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
#else
        curl_easy_setopt(c, CURLOPT_CAINFO, caBundlePath.c_str());
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
#endif

        CURLcode rc = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);

        if (rc != CURLE_OK) {
            err = curl_easy_strerror(rc);
            return false;
        }
        // Stremio returns JSON error bodies on 4xx; let the caller parse those.
        if (code >= 500) {
            err = "Stremio server error (HTTP " + std::to_string(code) + ")";
            return false;
        }
        return true;
    };
}

}  // namespace stremio
