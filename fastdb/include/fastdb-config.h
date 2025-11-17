#pragma once
//FastVectorDB:A fast vector database for local cache
//Author: wenyongning@njnu.edu.cn
#include <stdlib.h>
#include <stdio.h>
#include <cstring>
namespace wx{
    typedef          char   i8;
    typedef unsigned char   u8;
    typedef unsigned int    i32;
    typedef unsigned int    u32;
    typedef          short  i16;
    typedef unsigned short  u16;
    typedef float           f32;
    typedef double          f64;
    typedef u16             uchar_t;
}

#ifdef SWIG
    #define fastdb_api
#else
    #ifdef _WIN32
        #ifdef FASTDB_EXPORT
            #define fastdb_api __declspec(dllexport)
        #else
            #define fastdb_api __declspec(dllimport)
        #endif
    #else
        #define fastdb_api __attribute__((visibility("default")))
    #endif
#endif

