#pragma once
#include <dxgiformat.h>
#include <dxgi.h>
#include <d3d11.h>

namespace dxvk {
  using NTSTATUS = LONG;

  // Slightly modified definitions...
  struct D3DKMT_CREATEDCFROMMEMORY {
    void*         pMemory;
    DXGI_FORMAT Format;
    UINT          Width;
    UINT          Height;
    UINT          Pitch;
    HDC           hDeviceDc;
    PALETTEENTRY* pColorTable;
    HDC           hDc;
    HANDLE        hBitmap;
  };

  struct D3DKMT_DESTROYDCFROMMEMORY {
    HDC    hDC     = nullptr;
    HANDLE hBitmap = nullptr;
  };

  using D3DKMTCreateDCFromMemoryType  = NTSTATUS(STDMETHODCALLTYPE*) (D3DKMT_CREATEDCFROMMEMORY*);
  NTSTATUS D3DKMTCreateDCFromMemory (D3DKMT_CREATEDCFROMMEMORY*  Arg1);

  using D3DKMTDestroyDCFromMemoryType = NTSTATUS(STDMETHODCALLTYPE*) (D3DKMT_DESTROYDCFROMMEMORY*);
  NTSTATUS D3DKMTDestroyDCFromMemory(D3DKMT_DESTROYDCFROMMEMORY* Arg1);

}