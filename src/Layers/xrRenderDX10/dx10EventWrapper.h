#pragma once

// L"" #Name: MSVC accepts L#Name, clang reads L as an identifier
#define PIX_EVENT(Name) dxPixEventWrapper pixEvent##Name(L"" #Name)

class dxPixEventWrapper
{
public:
    dxPixEventWrapper(LPCWSTR wszName);
    ~dxPixEventWrapper();
}; 