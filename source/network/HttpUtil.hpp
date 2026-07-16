#pragma once
#include <string>

const char* GetContentType(const std::string& filename);
int  SendHttpResponse(int fd, int status, const char* contentType,
                      const char* body, int bodyLen);
int  SendHttpHeaders(int fd, const char* contentType,
                     const char* contentDisposition, long contentLength);
int  ParseFileIndex(const std::string& path);
