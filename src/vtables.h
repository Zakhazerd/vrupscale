#pragma once
// Method addresses read from live D3D11 objects by name (C/CINTERFACE), so no hand-counted
// vtable indices are involved.
#ifdef __cplusplus
extern "C" {
#endif

typedef struct VuCtxFns {
    void *PSSetShaderResources;
    void *CSSetShaderResources;
    void *Draw;
    void *DrawIndexed;
    void *DrawInstanced;
    void *DrawIndexedInstanced;
    void *DrawAuto;
    void *DrawInstancedIndirect;
    void *DrawIndexedInstancedIndirect;
    void *ClearState;
    void *CopyResource;
    void *CopySubresourceRegion;
    void *CopySubresourceRegion1;   /* ID3D11DeviceContext1, may be NULL */
    void *FinishCommandList;
    void *ExecuteCommandList;
    void **vtbl;                    /* the ID3D11DeviceContext vtable itself */
    int vtblCount;                  /* number of ID3D11DeviceContext methods in it */
} VuCtxFns;

typedef struct VuDevFns {
    void *OpenSharedResource;
    void *OpenSharedResource1;      /* ID3D11Device1, may be NULL */
    void *OpenSharedResourceByName; /* ID3D11Device1, may be NULL */
    void *CreateDeferredContext;
    void *CreateDeferredContext1;   /* ID3D11Device1, may be NULL */
} VuDevFns;

/* SetMultithreadProtected of ID3D11Multithread (from a context) and ID3D10Multithread (from a
   device); either may be NULL. */
typedef struct VuMtFns {
    void *SetMultithreadProtected11;
    void *SetMultithreadProtected10;
} VuMtFns;

void vu_get_ctx_fns(void *ctx, void *ctx1, VuCtxFns *out);
void vu_get_dev_fns(void *dev, void *dev1, VuDevFns *out);
void vu_get_mt_fns(void *mt11, void *mt10, VuMtFns *out);
/* Reads a method of ID3D11Multithread / ID3D10Multithread by name (C interface). */
int vu_mt11_get(void *mt11);
int vu_mt11_set(void *mt11, int on);

#ifdef __cplusplus
}
#endif
