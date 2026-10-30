#pragma once
/* Vtable slot indices, computed in vtbl.c from the SDK C interface declarations. */
#define VT_CTX_LIST(X) \
    X(VSSetConstantBuffers) X(PSSetConstantBuffers) X(CSSetConstantBuffers) X(GSSetConstantBuffers) \
    X(HSSetConstantBuffers) X(DSSetConstantBuffers) \
    X(VSSetConstantBuffers1) X(PSSetConstantBuffers1) X(CSSetConstantBuffers1) \
    X(VSSetShader) X(PSSetShader) X(CSSetShader) X(GSSetShader) \
    X(Map) X(Unmap) X(UpdateSubresource) X(UpdateSubresource1) \
    X(OMSetRenderTargets) X(OMSetRenderTargetsAndUnorderedAccessViews) X(RSSetViewports) \
    X(ClearRenderTargetView) X(ClearDepthStencilView) \
    X(Draw) X(DrawIndexed) X(DrawInstanced) X(DrawIndexedInstanced) \
    X(DrawIndexedInstancedIndirect) X(DrawInstancedIndirect) X(Dispatch) X(DispatchIndirect) \
    X(ExecuteCommandList) X(FinishCommandList) X(CopyResource) X(CopySubresourceRegion) X(ResolveSubresource)

#define VT_DEV_LIST(X) \
    X(CreateVertexShader) X(CreatePixelShader) X(CreateComputeShader) X(CreateGeometryShader) \
    X(CreateHullShader) X(CreateDomainShader) X(CreateDeferredContext)

#ifdef __cplusplus
extern "C" {
#endif
#define MEA_DECL_CTX(m) extern const int VT_CTX_##m;
#define MEA_DECL_DEV(m) extern const int VT_DEV_##m;
VT_CTX_LIST(MEA_DECL_CTX)
VT_DEV_LIST(MEA_DECL_DEV)
#ifdef __cplusplus
}
#endif
