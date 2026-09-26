#pragma once

#include <d3d12.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_hooks.h>

// These are the exports from SL library
typedef HRESULT(WINAPI* PFunCreateDXGIFactory)(REFIID, void**);
typedef HRESULT(WINAPI* PFunCreateDXGIFactory1)(REFIID, void**);
typedef HRESULT(WINAPI* PFunCreateDXGIFactory2)(UINT, REFIID, void**);
typedef HRESULT(WINAPI* PFunDXGIGetDebugInterface1)(UINT, REFIID, void**);
typedef HRESULT(WINAPI* PFunD3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef sl::Result(__stdcall* PFunSlGetUpgradeInterface)(void**);


// Map functions from SL and use them instead of standard DXGI/D3D12 API
extern PFunCreateDXGIFactory slCreateDXGIFactory;
extern PFunCreateDXGIFactory1 slCreateDXGIFactory1;
extern PFunCreateDXGIFactory2 slCreateDXGIFactory2;
extern PFunDXGIGetDebugInterface1 slDXGIGetDebugInterface1;
extern PFunD3D12CreateDevice slD3D12CreateDevice;
extern PFunSlGetUpgradeInterface slGetUpgradeInterface;