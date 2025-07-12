#ifndef TSTRING_H
#define TSTRING_H
#pragma once

#include <Windows.h>
#include <string>

/*
* ======================================================================
* NOTICE: This code contains substantial AI-generated content based on
* human-provided prompts, specifications, or design inputs.
*
* The code is provided as-is without warranty or guarantee of accuracy.
* AI-generated portions have not been thoroughly reviewed and may
* contain errors, logical flaws, or incomplete implementations.
*
* You are responsible for thorough review, testing, and validation
* before use. Exercise caution and comprehensive testing practices,
* as AI-generated code may introduce subtle issues not immediately
* apparent.
* ======================================================================
*/

#ifndef UNICODE
class tstring: public std::string {
public:
    tstring() = default;
    tstring(const std::string& str) : std::string(str) {}
    tstring(const char* str) : std::string(str) {}

    tstring(const std::wstring& str) {
        if (str.empty()) {
            static_cast<std::string&>(*this) = "";
            return;
        }
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &str[0], (int)str.length(), NULL, 0, NULL, NULL);
        if (size_needed > 0) {
            std::string strTo(size_needed, 0);
            WideCharToMultiByte(CP_UTF8, 0, &str[0], (int)str.length(), &strTo[0], size_needed, NULL, NULL);
            static_cast<std::string&>(*this) = strTo;
        } else {
            static_cast<std::string&>(*this) = ""; // Error or empty
        }
    }

    tstring(const wchar_t* str) {
        if (!str || str[0] == L'\0') {
            static_cast<std::string&>(*this) = "";
            return;
        }
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL); // -1 for null-terminated
        if (size_needed > 0) {
            std::string strTo(size_needed -1, 0); // -1 to exclude null terminator from std::string internal length
            WideCharToMultiByte(CP_UTF8, 0, str, -1, &strTo[0], size_needed -1, NULL, NULL);
            static_cast<std::string&>(*this) = strTo;
        } else {
            static_cast<std::string&>(*this) = ""; // Error or empty
        }
    }

    tstring(const tstring& other) : std::string(other) {}
    tstring(tstring&& other) noexcept : std::string(std::move(other)) {}

    tstring& operator=(const std::string& str) {
        static_cast<std::string&>(*this) = str;
        return *this;
    }

    tstring& operator=(const std::wstring& str) {
        if (str.empty()) {
            static_cast<std::string&>(*this) = "";
            return *this;
        }
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &str[0], (int)str.length(), NULL, 0, NULL, NULL);
        if (size_needed > 0) {
            std::string strTo(size_needed, 0);
            WideCharToMultiByte(CP_UTF8, 0, &str[0], (int)str.length(), &strTo[0], size_needed, NULL, NULL);
            static_cast<std::string&>(*this) = strTo;
        } else {
            static_cast<std::string&>(*this) = ""; // Error or empty
        }
        return *this;
    }

    tstring& operator=(const char* str) {
        static_cast<std::string&>(*this) = str;
        return *this;
    }

    tstring& operator=(const wchar_t* str) {
        if (!str || str[0] == L'\0') {
            static_cast<std::string&>(*this) = "";
            return *this;
        }
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL);
        if (size_needed > 0) {
            std::string strTo(size_needed - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, str, -1, &strTo[0], size_needed - 1, NULL, NULL);
            static_cast<std::string&>(*this) = strTo;
        } else {
            static_cast<std::string&>(*this) = "";
        }
        return *this;
    }
};
#else
class tstring: public std::wstring {
public:
    tstring() = default;
    tstring(const std::wstring& str) : std::wstring(str) {}
    tstring(const wchar_t* str) : std::wstring(str) {}

    tstring(const std::string& str) {
        if (str.empty()) {
            static_cast<std::wstring&>(*this) = L"";
            return;
        }
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.length(), NULL, 0);
        if (size_needed > 0) {
            std::wstring wstrTo(size_needed, 0);
            MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.length(), &wstrTo[0], size_needed);
            static_cast<std::wstring&>(*this) = wstrTo;
        } else {
            static_cast<std::wstring&>(*this) = L"";
        }
    }

    tstring(const char* str) {
        if (!str || str[0] == '\0') {
            static_cast<std::wstring&>(*this) = L"";
            return;
        }
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0); 
        if (size_needed > 0) {
            std::wstring wstrTo(size_needed -1, 0);
            MultiByteToWideChar(CP_UTF8, 0, str, -1, &wstrTo[0], size_needed -1);
            static_cast<std::wstring&>(*this) = wstrTo;
        } else {
            static_cast<std::wstring&>(*this) = L"";
        }
    }

    tstring(const tstring& other) : std::wstring(other) {}
    tstring(tstring&& other) noexcept : std::wstring(std::move(other)) {}

    tstring& operator=(const std::wstring& str) {
        static_cast<std::wstring&>(*this) = str;
        return *this;
    }

    tstring& operator=(const std::string& str) {
        if (str.empty()) {
            static_cast<std::wstring&>(*this) = L"";
            return *this;
        }
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.length(), NULL, 0);
        if (size_needed > 0) {
            std::wstring wstrTo(size_needed, 0);
            MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.length(), &wstrTo[0], size_needed);
            static_cast<std::wstring&>(*this) = wstrTo;
        } else {
            static_cast<std::wstring&>(*this) = L"";
        }
        return *this;
    }

    tstring& operator=(const wchar_t* str) {
        static_cast<std::wstring&>(*this) = str;
        return *this;
    }

    tstring& operator=(const char* str) {
        if (!str || str[0] == '\0') {
            static_cast<std::wstring&>(*this) = L"";
            return *this;
        }
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
        if (size_needed > 0) {
            std::wstring wstrTo(size_needed - 1, 0);
            MultiByteToWideChar(CP_UTF8, 0, str, -1, &wstrTo[0], size_needed - 1);
            static_cast<std::wstring&>(*this) = wstrTo;
        } else {
            static_cast<std::wstring&>(*this) = L"";
        }
        return *this;
    }
};
#endif

#endif