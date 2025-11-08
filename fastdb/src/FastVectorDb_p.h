#pragma once
#ifndef __FAST_VECTOR_DB_P_H__
#define __FAST_VECTOR_DB_P_H__
#include "fastdb.h"
#include "FastVectorDbLayer_p.h"
#include <vector>
using namespace std;
namespace wx{
    class FastVectorDb::Impl{
    public:
        Impl(void* pdata,size_t size,fnFreeDbBuffer fnFreeBuffer,void* cookie);
       ~Impl();
        unsigned              getLayerCount();
        FastVectorDbLayer*    getLayer(unsigned ix);
        FastVectorDbFeature*  tryGetFeature(FastVectorDbFeatureRef* ref);
        chunk_data_t          buffer();
    private:
        vector<FastVectorDbLayer*> m_layers;
        void*   m_pdata;
        size_t  m_size;
        fnFreeDbBuffer m_fnFreeBuffer;
        void* m_cookie;
        bool    m_mask_check_ok;
        friend class FastVectorDb;
    };
}
#endif
