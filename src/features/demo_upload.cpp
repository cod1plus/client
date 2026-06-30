// demo auto-upload: thread polls main/demos/, POSTs each stable .dm_1.
// no engine hook for /record yet, user triggers recording manually.

#include "features/demo_upload.h"
#include "core/toast.h"
#include "core/logger.h"

#include <wininet.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string>

namespace patches {

DemoUploadConfig g_demo_upload_config = {
    /* enable              */ false,
    /* upload_url          */ "",
    /* delete_after_upload */ false,
    /* show_toasts         */ true,
    /* poll_interval_sec   */ 10,
};

namespace {

HANDLE g_wake_event = NULL;  // manual-reset

bool resolve_demos_dir(char* out_path, size_t out_size) {
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) return false;
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(exe, exe_path, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return false;
    char* slash = strrchr(exe_path, '\\');
    if (!slash) return false;
    *slash = '\0';
    int n = snprintf(out_path, out_size, "%s\\main\\demos", exe_path);
    return n > 0 && (size_t)n < out_size;
}

// POST file as body, true on 2xx
bool http_post_file(const char* url, const char* file_path,
                    char* err_buf, size_t err_size) {
    FILE* fp = fopen(file_path, "rb");
    if (!fp) {
        snprintf(err_buf, err_size, "cannot open %s", file_path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fp);
        snprintf(err_buf, err_size, "empty file");
        return false;
    }

    char host[256] = {0};
    char path[512] = "/";
    bool https = false;
    if (strncmp(url, "https://", 8) == 0) {
        https = true;
        const char* h = url + 8;
        const char* p = strchr(h, '/');
        size_t hlen = p ? (size_t)(p - h) : strlen(h);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, h, hlen); host[hlen] = '\0';
        if (p) strncpy(path, p, sizeof(path) - 1);
    } else if (strncmp(url, "http://", 7) == 0) {
        const char* h = url + 7;
        const char* p = strchr(h, '/');
        size_t hlen = p ? (size_t)(p - h) : strlen(h);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, h, hlen); host[hlen] = '\0';
        if (p) strncpy(path, p, sizeof(path) - 1);
    } else {
        fclose(fp);
        snprintf(err_buf, err_size, "invalid url scheme");
        return false;
    }

    HINTERNET h_inet = InternetOpenA("cod1reloaded demo upload",
                                     INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!h_inet) {
        fclose(fp);
        snprintf(err_buf, err_size, "InternetOpen failed");
        return false;
    }

    HINTERNET h_conn = InternetConnectA(h_inet, host,
        https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT,
        NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!h_conn) {
        InternetCloseHandle(h_inet);
        fclose(fp);
        snprintf(err_buf, err_size, "InternetConnect failed");
        return false;
    }

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (https) flags |= INTERNET_FLAG_SECURE;
    HINTERNET h_req = HttpOpenRequestA(h_conn, "POST", path, NULL, NULL,
                                       NULL, flags, 0);
    if (!h_req) {
        InternetCloseHandle(h_conn);
        InternetCloseHandle(h_inet);
        fclose(fp);
        snprintf(err_buf, err_size, "HttpOpenRequest failed");
        return false;
    }

    const char* hdr = "Content-Type: application/octet-stream\r\n";

    INTERNET_BUFFERSA buf_in = {};
    buf_in.dwStructSize = sizeof(buf_in);
    buf_in.lpcszHeader = hdr;
    buf_in.dwHeadersLength = (DWORD)strlen(hdr);
    buf_in.dwBufferTotal = (DWORD)size;

    if (!HttpSendRequestExA(h_req, &buf_in, NULL, 0, 0)) {
        InternetCloseHandle(h_req);
        InternetCloseHandle(h_conn);
        InternetCloseHandle(h_inet);
        fclose(fp);
        snprintf(err_buf, err_size, "HttpSendRequestEx failed (err=%lu)", GetLastError());
        return false;
    }

    char chunk[8192];
    size_t total_sent = 0;
    while (!feof(fp)) {
        size_t r = fread(chunk, 1, sizeof(chunk), fp);
        if (r == 0) break;
        DWORD written = 0;
        if (!InternetWriteFile(h_req, chunk, (DWORD)r, &written) || written != r) {
            InternetCloseHandle(h_req);
            InternetCloseHandle(h_conn);
            InternetCloseHandle(h_inet);
            fclose(fp);
            snprintf(err_buf, err_size, "InternetWriteFile failed at %zu bytes", total_sent);
            return false;
        }
        total_sent += r;
    }
    fclose(fp);

    if (!HttpEndRequestA(h_req, NULL, 0, 0)) {
        InternetCloseHandle(h_req);
        InternetCloseHandle(h_conn);
        InternetCloseHandle(h_inet);
        snprintf(err_buf, err_size, "HttpEndRequest failed");
        return false;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    HttpQueryInfoA(h_req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &status, &status_size, NULL);

    InternetCloseHandle(h_req);
    InternetCloseHandle(h_conn);
    InternetCloseHandle(h_inet);

    if (status >= 200 && status < 300) {
        return true;
    }
    snprintf(err_buf, err_size, "HTTP %lu", status);
    return false;
}

void scan_and_upload(const char* demos_dir,
                     std::unordered_map<std::string, long>& size_cache) {
    char search[MAX_PATH];
    snprintf(search, sizeof(search), "%s\\*.dm_1", demos_dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s\\%s", demos_dir, fd.cFileName);

        char uploaded_marker[MAX_PATH];
        snprintf(uploaded_marker, sizeof(uploaded_marker), "%s.uploaded", full_path);
        if (GetFileAttributesA(uploaded_marker) != INVALID_FILE_ATTRIBUTES) continue;

        // skip until size stable across two polls (engine still writing)
        const long current_size = (long)fd.nFileSizeLow;
        auto it = size_cache.find(full_path);
        if (it == size_cache.end() || it->second != current_size) {
            size_cache[full_path] = current_size;
            continue;
        }

        if (current_size <= 0) continue;

        logger::logf("demo_upload: uploading %s (%ld bytes)", fd.cFileName, current_size);
        if (g_demo_upload_config.show_toasts) {
            char body[256];
            snprintf(body, sizeof(body), "Uploading %s (%ld KB)...",
                     fd.cFileName, current_size / 1024);
            toast_show("Demo upload", body, ToastType::Info);
        }

        char err[256] = {0};
        bool ok = http_post_file(g_demo_upload_config.upload_url, full_path,
                                 err, sizeof(err));
        if (ok) {
            logger::logf("demo_upload: SUCCESS %s", fd.cFileName);
            if (g_demo_upload_config.show_toasts) {
                char body[256];
                snprintf(body, sizeof(body), "%s uploaded successfully", fd.cFileName);
                toast_show("Demo uploaded", body, ToastType::Info);
            }
            if (g_demo_upload_config.delete_after_upload) {
                DeleteFileA(full_path);
            } else {
                FILE* mfp = fopen(uploaded_marker, "w");
                if (mfp) {
                    fprintf(mfp, "uploaded ok at %lu\n", (unsigned long)GetTickCount());
                    fclose(mfp);
                }
            }
            size_cache.erase(full_path);
        } else {
            logger::logf("demo_upload: FAILED %s - %s", fd.cFileName, err);
            if (g_demo_upload_config.show_toasts) {
                char body[256];
                snprintf(body, sizeof(body), "%s upload failed: %s", fd.cFileName, err);
                toast_show("Demo upload failed", body, ToastType::Error);
            }
            // leave file, retry next poll
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

DWORD WINAPI demo_upload_thread(LPVOID) {
    if (!g_demo_upload_config.enable) {
        logger::logf("demo_upload: disabled");
        return 0;
    }
    if (g_demo_upload_config.upload_url[0] == '\0') {
        logger::logf("demo_upload: enable=true but upload_url empty - skip");
        return 0;
    }

    char demos_dir[MAX_PATH];
    if (!resolve_demos_dir(demos_dir, sizeof(demos_dir))) {
        logger::logf("demo_upload: cannot resolve demos directory");
        return 0;
    }
    logger::logf("demo_upload: watching %s (url=%s, poll=%ds)",
                 demos_dir, g_demo_upload_config.upload_url,
                 g_demo_upload_config.poll_interval_sec);

    std::unordered_map<std::string, long> size_cache;

    int interval = g_demo_upload_config.poll_interval_sec;
    if (interval <= 0) {
        scan_and_upload(demos_dir, size_cache);
        return 0;
    }

    while (true) {
        scan_and_upload(demos_dir, size_cache);
        DWORD wait_ms = (DWORD)interval * 1000;
        if (g_wake_event) {
            WaitForSingleObject(g_wake_event, wait_ms);  // early wake on trigger_now
            ResetEvent(g_wake_event);
        } else {
            Sleep(wait_ms);
        }
    }
}

}  // namespace

void demo_upload_start() {
    if (!g_wake_event) {
        g_wake_event = CreateEventA(NULL, TRUE /*manual reset*/, FALSE, NULL);
    }
    HANDLE h = CreateThread(NULL, 0, demo_upload_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

void demo_upload_trigger_now() {
    if (g_wake_event) {
        SetEvent(g_wake_event);
        logger::logf("demo_upload: triggered immediate scan");
    }
}

}  // namespace patches
