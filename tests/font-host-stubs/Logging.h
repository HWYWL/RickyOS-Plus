#pragma once
template<class... Args> inline void fontHostLog(const char*, Args&&...) {}
#define LOG_DBG(tag,...) fontHostLog(__VA_ARGS__)
#define LOG_ERR(tag,...) fontHostLog(__VA_ARGS__)
