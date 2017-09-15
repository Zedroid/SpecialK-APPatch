/**
 * This file is part of Special K.
 *
 * Special K is free software : you can redistribute it
 * and/or modify it under the terms of the GNU General Public License
 * as published by The Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * Special K is distributed in the hope that it will be useful,
 *
 * But WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Special K.
 *
 *   If not, see <http://www.gnu.org/licenses/>.
 *
**/

#include <d3d9.h>

#include <SpecialK/d3d9_backend.h>
#include <SpecialK/d3d9/texmgr.h>
#include <SpecialK/config.h>
#include <SpecialK/hooks.h>
#include <SpecialK/log.h>
#include <SpecialK/utility.h>
#include <SpecialK/framerate.h>
#include <SpecialK/core.h>
#include <process.h>

#include <cstdint>
#include <algorithm>

#include "command.h"

#include <lzma/7z.h>
#include <lzma/7zAlloc.h>
#include <lzma/7zBuf.h>
#include <lzma/7zCrc.h>
#include <lzma/7zFile.h>
#include <lzma/7zVersion.h>

static ISzAlloc g_Alloc = { SzAlloc, SzFree };

#include <atlbase.h>
#include <memory>
#include <ctime>
#include <map>
#include <set>
#include <queue>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include <comdef.h>

using namespace SK::D3D9;

iSK_Logger tex_log;

extern std::wstring SK_D3D11_res_root;

static int d3d9_max_cache_in_mib = 3072;

static D3DXCreateTextureFromFileInMemoryEx_pfn D3DXCreateTextureFromFileInMemoryEx_Original = nullptr;

static BeginScene_pfn                          D3D9BeginScene_Original                      = nullptr;
static EndScene_pfn                            D3D9EndScene_Original                        = nullptr;
static SetRenderState_pfn                      D3D9SetRenderState_Original                  = nullptr;

static StretchRect_pfn                         D3D9StretchRect_Original                     = nullptr;
static CreateTexture_pfn                       D3D9CreateTexture_Original                   = nullptr;
static CreateRenderTarget_pfn                  D3D9CreateRenderTarget_Original              = nullptr;
static CreateDepthStencilSurface_pfn           D3D9CreateDepthStencilSurface_Original       = nullptr;

static SetTexture_pfn                          D3D9SetTexture_Original                      = nullptr;
static SetRenderTarget_pfn                     D3D9SetRenderTarget_Original                 = nullptr;
static SetDepthStencilSurface_pfn              D3D9SetDepthStencilSurface_Original          = nullptr;
extern SetSamplerState_pfn                     D3D9SetSamplerState_Original;

using  QueryPerformanceCounter_pfn = BOOL (WINAPI *)( _Out_ LARGE_INTEGER *lpPerformanceCount );
extern QueryPerformanceCounter_pfn QueryPerformanceCounter_Original;

// D3DXSaveSurfaceToFile issues a StretchRect, but we don't want to log that...
bool dumping          = false;
bool __remap_textures = true;
bool __need_purge     = false;
bool __log_used       = false;
bool __show_cache     = true;//false;

// Cleanup
std::queue <std::wstring>              screenshots_to_delete;

// Textures that are missing mipmaps
std::set   <IDirect3DBaseTexture9 *>   incomplete_textures;

SK::D3D9::TextureThreadPool *SK::D3D9::resample_pool = nullptr;
SK::D3D9::StreamSplitter     SK::D3D9::stream_pool;

TextureManager               SK::D3D9::tex_mgr;

std::unordered_map <uint32_t, IDirect3DTexture9*> injected_textures;
std::unordered_map <uint32_t, float>              injected_load_times;
std::unordered_map <uint32_t, size_t>             injected_sizes;

SK_ThreadSafe_HashSet <IDirect3DSurface9 *> outstanding_screenshots;
                                            // Not excellent screenshots, but screenhots
                                            //   that aren't finished yet and we can't reset
                                            //     the D3D9 device because of.

size_t
SK::D3D9::TextureManager::getTextureArchives (std::vector <std::wstring>& arcs)
{
  arcs = archives;

  return arcs.size ();
}

size_t
SK::D3D9::TextureManager::getInjectableTextures (SK::D3D9::TexList& texture_list) const
{
  for ( auto&& it : injectable_textures ) 
  {
    texture_list.emplace_back (std::make_pair (it.first, it.second));
  }

  return texture_list.size ();
}

SK::D3D9::TexRecord&
SK::D3D9::TextureManager::getInjectableTexture (uint32_t checksum)
{
  static TexRecord  nulref     = {    };
         TexRecord& injectable = nulref;

  injector.lockInjection ();

  bool new_tex = false;

  if (! injectable_textures.count (checksum))
    new_tex = true;

  injectable =
    injectable_textures [checksum];

  if (new_tex)
    injectable = { };

  injector.unlockInjection ();

  return injectable;
}

#if 0
COM_DECLSPEC_NOTHROW
__declspec (noinline)
HRESULT
STDMETHODCALLTYPE
D3D9StretchRect_Detour (      IDirect3DDevice9    *This,
                              IDirect3DSurface9   *pSourceSurface,
                        const RECT                *pSourceRect,
                              IDirect3DSurface9   *pDestSurface,
                        const RECT                *pDestRect,
                              D3DTEXTUREFILTERTYPE Filter )
{
#if 0
  if (tzf::RenderFix::tracer.log && (! dumping))
  {
    RECT source, dest;

    if (pSourceRect == nullptr) {
      D3DSURFACE_DESC desc;
      pSourceSurface->GetDesc (&desc);
      source.left   = 0;
      source.top    = 0;
      source.bottom = desc.Height;
      source.right  = desc.Width;
    } else
      source = *pSourceRect;

    if (pDestRect == nullptr) {
      D3DSURFACE_DESC desc;
      pDestSurface->GetDesc (&desc);
      dest.left   = 0;
      dest.top    = 0;
      dest.bottom = desc.Height;
      dest.right  = desc.Width;
    } else
      dest = *pDestRect;

    dll_log->Log ( L"[FrameTrace] StretchRect      - "
                   L"%s[%lu,%lu/%lu,%lu] ==> %s[%lu,%lu/%lu,%lu]",
                   pSourceRect != nullptr ?
                     L" " : L" *",
                   source.left, source.top, source.right, source.bottom,
                   pDestRect != nullptr ?
                     L" " : L" *",
                   dest.left,   dest.top,   dest.right,   dest.bottom );
  }
#endif

  dumping = false;

  return D3D9StretchRect (This, pSourceSurface, pSourceRect,
                                         pDestSurface,   pDestRect,
                                         Filter);
}
#endif

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9CreateRenderTarget_Detour (IDirect3DDevice9     *This,
                                UINT                  Width,
                                UINT                  Height,
                                D3DFORMAT             Format,
                                D3DMULTISAMPLE_TYPE   MultiSample,
                                DWORD                 MultisampleQuality,
                                BOOL                  Lockable,
                                IDirect3DSurface9   **ppSurface,
                                HANDLE               *pSharedHandle)
{
  tex_log.Log (L"[Unexpected][!] IDirect3DDevice9::CreateRenderTarget (%lu, %lu, "
                       L"%lu, %lu, %lu, %lu, %08Xh, %08Xh)",
                  Width, Height, Format, MultiSample, MultisampleQuality,
                  Lockable, ppSurface, pSharedHandle);

  return D3D9CreateRenderTarget_Original (This, Width, Height, Format,
                                          MultiSample, MultisampleQuality,
                                          Lockable, ppSurface, pSharedHandle);
}

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9CreateDepthStencilSurface_Detour (IDirect3DDevice9     *This,
                                      UINT                  Width,
                                      UINT                  Height,
                                      D3DFORMAT             Format,
                                      D3DMULTISAMPLE_TYPE   MultiSample,
                                      DWORD                 MultisampleQuality,
                                      BOOL                  Discard,
                                      IDirect3DSurface9   **ppSurface,
                                      HANDLE               *pSharedHandle)
{
  tex_log.Log (L"[Unexpected][!] IDirect3DDevice9::CreateDepthStencilSurface (%lu, %lu, "
                       L"%lu, %lu, %lu, %lu, %08Xh, %08Xh)",
                  Width, Height, Format, MultiSample, MultisampleQuality,
                  Discard, ppSurface, pSharedHandle);

  return D3D9CreateDepthStencilSurface_Original (This, Width, Height, Format,
                                                 MultiSample, MultisampleQuality,
                                                 Discard, ppSurface, pSharedHandle);
}

int
SK::D3D9::TextureManager::numInjectedTextures (void) const
{
  return ReadNoFence (&injected_count);
}

int64_t
SK::D3D9::TextureManager::cacheSizeInjected (void) const
{
  return ReadNoFence64 (&injected_size);
}

int64_t
SK::D3D9::TextureManager::cacheSizeBasic (void) const
{
  return ReadNoFence64 (&basic_size);
}

int64_t
SK::D3D9::TextureManager::cacheSizeTotal (void) const
{
  return std::max (0LL, cacheSizeBasic ()) + std::max (0LL, cacheSizeInjected ());
}

bool
SK::D3D9::TextureManager::isRenderTarget (IDirect3DBaseTexture9* pTex) const
{
  return known.render_targets.count (pTex) != 0;
}

void
SK::D3D9::TextureManager::trackRenderTarget (IDirect3DBaseTexture9* pTex)
{
  if (! known.render_targets.count (pTex))
  {
    known.render_targets.try_emplace ( pTex,
                               (uint32_t)known.render_targets.size () );
  }
}

void
SK::D3D9::TextureManager::applyTexture (IDirect3DBaseTexture9* pTex)
{
  if (known.render_targets.count (pTex) != 0)
  {
    used.render_targets.emplace (pTex);
  }
}

bool
SK::D3D9::TextureManager::isUsedRenderTarget (IDirect3DBaseTexture9* pTex) const
{
  return used.render_targets.count (pTex) != 0;
}

void
SK::D3D9::TextureManager::resetUsedTextures (void)
{
  used.render_targets.clear ();
}

size_t
SK::D3D9::TextureManager::getUsedRenderTargets (std::vector <IDirect3DBaseTexture9 *>& targets) const
{
  targets = {
    used.render_targets.cbegin (), used.render_targets.cend ()
  };

  return targets.size ();
}

uint32_t
SK::D3D9::TextureManager::getRenderTargetCreationTime (IDirect3DBaseTexture9* pTex)
{
  if (known.render_targets.count (pTex))
    return known.render_targets [pTex];

  return 0xFFFFFFFFUL;
}

COM_DECLSPEC_NOTHROW
__declspec (noinline)
HRESULT
STDMETHODCALLTYPE
D3D9StretchRect_Detour (      IDirect3DDevice9    *This,
                               IDirect3DSurface9   *pSourceSurface,
                         const RECT                *pSourceRect,
                               IDirect3DSurface9   *pDestSurface,
                         const RECT                *pDestRect,
                               D3DTEXTUREFILTERTYPE Filter )
{
  dumping = false;

  return D3D9StretchRect_Original (This, pSourceSurface, pSourceRect,
                                         pDestSurface,   pDestRect,
                                         Filter);
}


std::set <UINT> SK::D3D9::active_samplers;

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9SetDepthStencilSurface_Detour (
                  _In_ IDirect3DDevice9  *This,
                  _In_ IDirect3DSurface9 *pNewZStencil
)
{
  // Ignore anything that's not the primary render device.
  if (This != SK_GetCurrentRenderBackend ().device)
  {
    return D3D9SetDepthStencilSurface_Original (This, pNewZStencil);
  }

  return D3D9SetDepthStencilSurface_Original (This, pNewZStencil);
}


uint32_t debug_tex_id      =   0UL;
uint32_t current_tex [256] = { 0ui32 };


COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9SetTexture_Detour (
                   _In_ IDirect3DDevice9      *This,
                   _In_ DWORD                  Sampler,
                   _In_ IDirect3DBaseTexture9 *pTexture
)
{
  if (pTexture == nullptr)
    return D3D9SetTexture_Original (This, Sampler, pTexture);

  //if (tzf::RenderFix::tracer.log) {
    //dll_log.Log ( L"[FrameTrace] SetTexture      - Sampler: %lu, pTexture: %ph",
    //               Sampler, pTexture );
  //}


  tex_mgr.applyTexture (pTexture);
  tracked_rt.active  = (pTexture == tracked_rt.tracking_tex);

  if (tracked_rt.active)
  {
    tracked_rt.vertex_shaders.emplace (Shaders.vertex.current.crc32c);
    tracked_rt.pixel_shaders.emplace  (Shaders.pixel.current.crc32c);
  }


  if (Shaders.vertex.current.crc32c == tracked_vs.crc32c)
    tracked_vs.current_textures [std::min (15UL, Sampler)] = pTexture;

  if (Shaders.pixel.current.crc32c == tracked_ps.crc32c)
    tracked_ps.current_textures [std::min (15UL, Sampler)] = pTexture;



  uint32_t tex_crc32c = 0x0;

  void* dontcare;
  if ( pTexture != nullptr &&
       pTexture->QueryInterface (IID_SKTextureD3D9, &dontcare) == S_OK )
  {
    auto* pSKTex =
      dynamic_cast <ISKTextureD3D9 *> (pTexture);

    current_tex [std::min (255UL, Sampler)] = pSKTex->tex_crc32c;

    //if (Shaders.vertex.current.crc32c == tracked_vs.crc32c)
    //    tracked_vs.current_textures [std::min (15UL, Sampler)] = pSKTex->tex_crc32c;
    //
    //if (Shaders.pixel.current.crc32c  == tracked_ps.crc32c)
    //    tracked_ps.current_textures [std::min (15UL, Sampler)] = pSKTex->tex_crc32c;

    if (pSKTex->tex_crc32c != 0x00)
    {
      tex_mgr.textures_used.emplace (pSKTex->tex_crc32c);

      ////tex_log.Log (L"Used: %x", pSKTex->tex_crc32c);
    }

    tex_crc32c =
      pSKTex->tex_crc32c;

    pTexture =
      pSKTex->use ();

    //
    // This is how blocking is implemented -- only do it when a texture that needs
    //                                          this feature is being applied.
    //
    while ( __remap_textures && pSKTex->must_block &&
                                pSKTex->pTexOverride == nullptr )
    {
      if (tex_mgr.injector.hasPendingLoads ())
      {
        tex_mgr.loadQueuedTextures ();
      }
    
      else
      {
        SwitchToThread ();
        //YieldProcessor ();
      }
    }

    pTexture = pSKTex->getDrawTexture ();
  }

#if 0
  if (pTexture != nullptr) tsf::RenderFix::active_samplers.insert (Sampler);
  else                     tsf::RenderFix::active_samplers.erase  (Sampler);
#endif

  bool clamp = false;

  if (Shaders.pixel.current.crc32c == tracked_ps.crc32c && tracked_ps.clamp_coords)
    clamp = true;

  if (Shaders.vertex.current.crc32c ==tracked_vs.crc32c && tracked_vs.clamp_coords)
    clamp = true;

  if ( clamp )
  {
    float fMin = -3.0f;

    D3D9SetSamplerState_Original (This, Sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
    D3D9SetSamplerState_Original (This, Sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
    D3D9SetSamplerState_Original (This, Sampler, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP );
    D3D9SetSamplerState_Original (This, Sampler, D3DSAMP_MIPMAPLODBIAS, *reinterpret_cast <DWORD *>(&fMin) );
  }

  return D3D9SetTexture_Original (This, Sampler, pTexture);
}

IDirect3DSurface9* pOld     = nullptr;

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9CreateTexture_Detour (IDirect3DDevice9    *This,
                           UINT                Width,
                           UINT                Height,
                           UINT                Levels,
                           DWORD               Usage,
                           D3DFORMAT           Format,
                           D3DPOOL             Pool,
                           IDirect3DTexture9 **ppTexture,
                           HANDLE             *pSharedHandle)
{
#if 0
  if (Usage == D3DUSAGE_RENDERTARGET)
  dll_log->Log (L" [!] IDirect3DDevice9::CreateTexture (%lu, %lu, %lu, %lu, "
                                                   L"%lu, %lu, %08Xh, %08Xh)",
                  Width, Height, Levels, Usage, Format, Pool, ppTexture,
                  pSharedHandle);
#endif

#if 0
  //if (config.textures.log) {
    tex_log->Log ( L"[Load Trace] >> Creating Texture: "
                  L"(%d x %d), Format: %s, Usage: [%s], Pool: %s",
                    Width, Height,
                      SK_D3D9_FormatToStr (Format),
                      SK_D3D9_UsageToStr  (Usage).c_str (),
                      SK_D3D9_PoolToStr   (Pool) );
  //}
//#endif
#endif

  int levels = Levels;

  if (SK_GetCurrentGameID () == SK_GAME_ID::YS_Seven)
  {
    if ( ( Usage & D3DUSAGE_RENDERTARGET ) )
    {
      if (Format == D3DFMT_R5G6B5 && ( Width != 2048 ) )
      {
        Format   = D3DFMT_X8R8G8B8;
#if 0
          Width  <<= 1;
          Height <<= 1;
#endif
      }
    }
  }

  HRESULT result = 
    D3D9CreateTexture_Original (This, Width, Height, levels, Usage,
                                Format, Pool, ppTexture, pSharedHandle);

  if ( SUCCEEDED (result) &&
       ( ( Usage & D3DUSAGE_RENDERTARGET ) || 
         ( Usage & D3DUSAGE_DEPTHSTENCIL ) /*||
         ( Usage & D3DUSAGE_DYNAMIC      )*/ ) )
  {
    tex_mgr.trackRenderTarget (*ppTexture);
  }

  else if (SUCCEEDED (result))
  {
    if (! SK::D3D9::tex_mgr.injector.isInjectionThread ())
    {
      static HMODULE hModSteamOverlay =
#ifndef _WIN64
        GetModuleHandle (L"gameoverlayrenderer.dll");
#else
        GetModuleHandle (L"gameoverlayrenderer64.dll");
#endif

      // Recent changes to the Steam overlay cause a million
      //   textures to be pre-loaded at start, we need to ignore
      //     them or the overlay will kill performance.
      HMODULE hModCaller = SK_GetCallingDLL ();

      if ( hModCaller != hModSteamOverlay &&
           hModCaller != SK_GetDLL () )
        new ISKTextureD3D9 (ppTexture, 0, 0x00);
    }
  }

  return result;
}

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9BeginScene_Detour (IDirect3DDevice9* This)
{
  // Ignore anything that's not the primary render device.
  if (This != SK_GetCurrentRenderBackend ().device)
  {
    dll_log.Log (L"[D3D9 BkEnd] >> WARNING: D3D9 BeginScene came from unknown IDirect3DDevice9! << ");

    return D3D9BeginScene_Original (This);
  }

  draw_state.draws = 0;

  HRESULT result =
    D3D9BeginScene_Original (This);

  return result;
}

#if 0
COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9EndScene_Detour (IDirect3DDevice9* This)
{
  // Ignore anything that's not the primary render device.
  if (This != tzf::RenderFix::pDevice) {
    return D3D9EndScene_Original (This);
  }
  return D3D9EndScene_Original (This);
}
#endif

#define __PTR_SIZE   sizeof LPCVOID
#define __PAGE_PRIVS PAGE_EXECUTE_READWRITE

#define D3D9_VIRTUAL_OVERRIDE(_Base,_Index,_Name,_Override,_Original,_Type) { \
  void** vftable = *(void***)*_Base;                                          \
                                                                              \
  if (vftable [_Index] != _Override) {                                        \
    DWORD dwProtect;                                                          \
                                                                              \
    VirtualProtect (&vftable [_Index], __PTR_SIZE, __PAGE_PRIVS, &dwProtect); \
                                                                              \
    /*dll_log->Log (L" Old VFTable entry for %s: %08Xh  (Memory Policy: %s)",*/\
                 /*L##_Name, vftable [_Index],                              */\
                 /*SK_DescribeVirtualProtectFlags (dwProtect));             */\
                                                                              \
    if (_Original == NULL)                                                    \
      _Original = (##_Type)vftable [_Index];                                  \
                                                                              \
    /*dll_log->Log (L"  + %s: %08Xh", L#_Original, _Original);*/               \
                                                                              \
    vftable [_Index] = _Override;                                             \
                                                                              \
    VirtualProtect (&vftable [_Index], __PTR_SIZE, dwProtect, &dwProtect);    \
                                                                              \
    /*dll_log->Log (L" New VFTable entry for %s: %08Xh  (Memory Policy: %s)\n",*/\
                  /*L##_Name, vftable [_Index],                               */\
                  /*SK_DescribeVirtualProtectFlags (dwProtect));              */\
  }                                                                           \
}

SK::D3D9::TextureWorkerThread::TextureWorkerThread (SK::D3D9::TextureThreadPool* pool)
{
  pool_ = pool;
  job_  = nullptr;

  control_.start =
    CreateEvent (nullptr, FALSE, FALSE, nullptr);
  control_.trim =
    CreateEvent (nullptr, FALSE, FALSE, nullptr);
  control_.shutdown =
    CreateEvent (nullptr, FALSE, FALSE, nullptr);

  thread_ =
    (HANDLE)_beginthreadex ( nullptr,
                               0,
                                 ThreadProc,
                                   this,
                                     0,
                                       &thread_id_ );
}

SK::D3D9::TextureWorkerThread::~TextureWorkerThread (void)
{
  shutdown ();

  WaitForSingleObject (thread_, INFINITE);

  CloseHandle (control_.shutdown);
  CloseHandle (control_.trim);
  CloseHandle (control_.start);

  CloseHandle (thread_);
}

bool
SK::D3D9::TextureManager::Injector::hasPendingLoads (void) const
{
  bool ret = false;

  return
    ( stream_pool.working () ||
        ( resample_pool != nullptr && resample_pool->working () ) );

//  EnterCriticalSection (&cs_tex_inject);
//  ret = (! finished_loads.empty ());
//  LeaveCriticalSection (&cs_tex_inject);

  return ret;
}

void
SK::D3D9::TextureManager::Injector::beginLoad (void)
{
  SK_TLS_Bottom ()->texture_management.injection_thread = TRUE;
}

void
SK::D3D9::TextureManager::Injector::endLoad (void)
{
  SK_TLS_Bottom ()->texture_management.injection_thread = FALSE;
}


bool
SK::D3D9::TextureManager::Injector::hasPendingStreams (void) const
{
  bool ret = false;

  
  if (ReadAcquire (&streaming) || stream_pool.queueLength () || (resample_pool && resample_pool->queueLength ()))
    ret = true;

  return ret;
}

bool
SK::D3D9::TextureManager::Injector::isStreaming (uint32_t checksum) const
{
  bool ret = false;

  lockStreaming ();

  if (textures_in_flight.count (checksum))
    ret = true;

  unlockStreaming ();

  return ret;
}

void
SK::D3D9::TextureManager::Injector::finishedStreaming (uint32_t checksum)
{
  lockStreaming ();

  if (isStreaming (checksum))
    textures_in_flight.erase (checksum);

  unlockStreaming ();
}

void
SK::D3D9::TextureManager::Injector::addTextureInFlight ( SK::D3D9::TexLoadRequest* load_op )
{
  lockStreaming ();

  textures_in_flight.emplace (std::make_pair (load_op->checksum, load_op));

  unlockStreaming ();
}

SK::D3D9::TexLoadRequest*
SK::D3D9::TextureManager::Injector::getTextureInFlight (uint32_t checksum)
{
  lockStreaming ();

  SK::D3D9::TexLoadRequest* pLoadRequest = nullptr;

  // What to do if this load finishes before the thing that acquired the lock is done?
  if (isStreaming (checksum))
    pLoadRequest = textures_in_flight [checksum];

  unlockStreaming ();

  return pLoadRequest;
}


HANDLE decomp_semaphore;

#include <SpecialK/tls.h>

// Keep a pool of memory around so that we are not allocating and freeing
//  memory constantly...
namespace streaming_memory {
  bool alloc (size_t len)
  {
    SK_TLS::tex_mgmt_s::stream_pool_s* mpool =
      &SK_TLS_Bottom ()->texture_management.streaming_memory;

    if (mpool->data_len < len)
    {
      if (mpool->data != nullptr)
        free (mpool->data);

      if (len < 8192 * 1024)
        mpool->data_len = 8192 * 1024;
      else
        mpool->data_len = len;

      mpool->data =
        malloc (mpool->data_len);

      mpool->data_age = timeGetTime ();

      if (mpool->data != nullptr)
      {
        return true;
      }

      else
      {
        mpool->data_len = 0;
        return false;
      }
    }

    else
    {
      return true;
    }
  }

  void*& data (void)
  {
    SK_TLS::tex_mgmt_s::stream_pool_s* mpool =
      &SK_TLS_Bottom ()->texture_management.streaming_memory;

    return mpool->data;
  }

  size_t& data_len (void)
  {
    SK_TLS::tex_mgmt_s::stream_pool_s* mpool =
      &SK_TLS_Bottom ()->texture_management.streaming_memory;

    return mpool->data_len;
  }

  uint32_t& data_age (void)
  {
    SK_TLS::tex_mgmt_s::stream_pool_s* mpool =
      &SK_TLS_Bottom ()->texture_management.streaming_memory;

    return mpool->data_age;
  }

  void trim (size_t max_size, uint32_t min_age)
  {
    SK_TLS::tex_mgmt_s::stream_pool_s* mpool =
      &SK_TLS_Bottom ()->texture_management.streaming_memory;

    if (  mpool->data_age < min_age )
    {
      if (mpool->data_len > max_size)
      {
        free (mpool->data);
             mpool->data = nullptr;

        if (max_size > 0)
          mpool->data = malloc (max_size);

        if (mpool->data != nullptr)
        {
          mpool->data_len = max_size;
          mpool->data_age = timeGetTime ();
        }

        else
        {
          mpool->data_len = 0;
          mpool->data_age = 0;
        }
      }
    }
  }
}

bool
SK::D3D9::TextureManager::isTextureBlacklisted (uint32_t/* checksum*/) const
{
  return false;

//  bool bRet = false;
//
//  injector.lockBlacklist ();
//
//  bRet = ( inject_blacklist.count (checksum) != 0 );
//
//  injector.unlockBlacklist ();
//
//  return bRet;
}

bool
SK::D3D9::TextureManager::isTextureInjectable (uint32_t checksum) const
{
  bool bRet = false;

  injector.lockInjection ();

  bRet = (injectable_textures.count (checksum) != 0);

  injector.unlockInjection ();

  return bRet;
}

bool
SK::D3D9::TextureManager::removeInjectableTexture (uint32_t checksum)
{
  bool bRet = false;

  injector.lockInjection ();

  bRet = (injectable_textures.count (checksum) != 0);

  if (bRet)
    injectable_textures.erase (checksum);

  injector.unlockInjection ();

  return bRet;
}

HRESULT
SK::D3D9::TextureManager::injectTexture (TexLoadRequest* load)
{
  D3DXIMAGE_INFO img_info = {    };
  bool           streamed =  false;
  size_t         size     =      0;
  HRESULT        hr       = E_FAIL;

  auto inject =
    injectable_textures.find (load->checksum);

  if (inject == injectable_textures.end ())
  {
    tex_log.Log ( L"[Inject Tex]  >> Load Request for Checksum: %X "
                  L"has no Injection Record !!",
                    load->checksum );

    return E_NOT_VALID_STATE;
  }

  const TexRecord* inj_tex =
    &(*inject).second;

  streamed =
    (inj_tex->method == Streaming);

  //
  // Load:  From Regular Filesystem
  //
  if ( inj_tex->archive == std::numeric_limits <unsigned int>::max () )
  {
    HANDLE hTexFile =
      CreateFile ( load->wszFilename,
                     GENERIC_READ,
                       FILE_SHARE_READ,
                         nullptr,
                           OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL |
                             FILE_FLAG_SEQUENTIAL_SCAN,
                               nullptr );

    DWORD read = 0UL;

    if (hTexFile != INVALID_HANDLE_VALUE)
    {
      size = GetFileSize (hTexFile, nullptr);

      if (streaming_memory::alloc (size))
      {
        load->pSrcData = streaming_memory::data ();

        ReadFile (hTexFile, load->pSrcData, (DWORD)size, &read, nullptr);

        load->SrcDataSize = read;

        if (streamed && size > (256 * 1024))
        {
          SetThreadPriority ( GetCurrentThread (),
                                THREAD_PRIORITY_BELOW_NORMAL |
                                THREAD_MODE_BACKGROUND_BEGIN );
        }

        D3DXGetImageInfoFromFileInMemory (
          load->pSrcData,
            load->SrcDataSize,
              &img_info );

        hr = D3DXCreateTextureFromFileInMemoryEx_Original (
          load->pDevice,
            load->pSrcData, load->SrcDataSize,
              D3DX_DEFAULT, D3DX_DEFAULT, img_info.MipLevels,
                0, D3DFMT_FROM_FILE,
                  D3DPOOL_DEFAULT,
                    D3DX_DEFAULT, D3DX_DEFAULT,
                      0,
                        &img_info, nullptr,
                          &load->pSrc );

        load->pSrcData = nullptr;
      }

      else {
        // OUT OF MEMORY ?!
      }

      CloseHandle (hTexFile);
    }
  }

  //
  // Load:  From (Compressed) Archive (.7z or .zip)
  //
  else
  {
    wchar_t       arc_name [MAX_PATH] = { };

    CFileInStream arc_stream          = { };
    CLookToRead   look_stream         = { };
    ISzAlloc      thread_alloc        = { };
    ISzAlloc      thread_tmp_alloc    = { };

    FileInStream_CreateVTable (&arc_stream);
    LookToRead_CreateVTable   (&look_stream, False);

    look_stream.realStream = &arc_stream.s;
    LookToRead_Init         (&look_stream);

    thread_alloc.Alloc     = SzAlloc;
    thread_alloc.Free      = SzFree;

    thread_tmp_alloc.Alloc = SzAllocTemp;
    thread_tmp_alloc.Free  = SzFreeTemp;

    CSzArEx      arc    = { };
                 size   = inj_tex->size;
    int          fileno = inj_tex->fileno;

    if (inj_tex->archive <= archives.size ())
      wcscpy (arc_name, archives [inj_tex->archive].c_str ());
    else
      wcscpy (arc_name, L"INVALID");

    if (streamed && size > (256 * 1024))
    {
      SetThreadPriority ( GetCurrentThread (),
                            THREAD_PRIORITY_LOWEST |
                            THREAD_MODE_BACKGROUND_BEGIN );
    }

    if (InFile_OpenW (&arc_stream.file, arc_name))
    {
      tex_log.Log ( L"[Inject Tex]  ** Cannot open archive file: %s",
                      arc_name );

      return E_FAIL;
    }

    SzArEx_Init (&arc);

    if (SzArEx_Open (&arc, &look_stream.s, &thread_alloc, &thread_tmp_alloc) != SZ_OK)
    {
      tex_log.Log ( L"[Inject Tex]  ** Cannot open archive file: %s",
                      arc_name );

      File_Close  (&arc_stream.file);
      SzArEx_Free (&arc, &thread_alloc);

      return E_INVALIDARG;
    }

    if (streaming_memory::alloc (size))
    {
      load->pSrcData = streaming_memory::data ();
      bool wait      = true;

      while (wait)
      {
        DWORD dwResult = WAIT_OBJECT_0;

        if (streamed && size > (256 * 1024))
        {
          dwResult =
            WaitForSingleObject ( decomp_semaphore, INFINITE );
        }

        switch (dwResult) 
        {
        case WAIT_OBJECT_0:
        {
          UInt32   block_idx     = 0xFFFFFFFF;
          auto*    out           = static_cast <Byte *> (streaming_memory::data     ());
          size_t   out_len       =                       streaming_memory::data_len ();
          size_t   offset        = 0;
          size_t   decomp_size   = 0;

          if (SZ_OK == SzArEx_Extract ( &arc,          &look_stream.s, fileno,
                                        &block_idx,    &out,        &out_len,
                                        &offset,       &decomp_size,
                                        &thread_alloc, &thread_tmp_alloc ) )
          {
            if (streamed && size > (96 * 1024))
              ReleaseSemaphore (decomp_semaphore, 1, nullptr);

            wait = false;

            load->pSrcData    = static_cast <Byte *> (streaming_memory::data ()) + offset;
            load->SrcDataSize = static_cast <UINT>   (decomp_size);

            if (SUCCEEDED ( D3DXGetImageInfoFromFileInMemory (
                              load->pSrcData,
                                load->SrcDataSize,
                                  &img_info )
                          )
               )
            {
              load->pSrc = nullptr;

              hr =
                D3DXCreateTextureFromFileInMemoryEx_Original (
                  load->pDevice,
                    load->pSrcData, load->SrcDataSize,
                      img_info.Width, img_info.Height, img_info.MipLevels,
                        0, img_info.Format,
                          D3DPOOL_DEFAULT,
                            D3DX_DEFAULT, D3DX_DEFAULT,
                              0,
                                &img_info, nullptr,
                                  &load->pSrc );
            }
          }

          else
          {
            tex_log.Log ( L"[  Tex. Mgr  ] Unable to read from 7-Zip File... for texture %x",
                          load->checksum );
          }
        } break;

        default:
          tex_log.Log ( L"[  Tex. Mgr  ] Unexpected Wait Status: %X (crc32=%x)",
                          dwResult,
                            load->checksum );
          wait = false;
          break; 
        }
      }

      load->pSrcData = nullptr;
    }

    File_Close  (&arc_stream.file);
    SzArEx_Free (&arc, &thread_alloc);
  }

  if (streamed && size > (256 * 1024))
  {
    SetThreadPriority ( GetCurrentThread (),
                          THREAD_MODE_BACKGROUND_END );
  }

  return hr;
}

CRITICAL_SECTION osd_cs            = { };
DWORD            last_queue_update =   0;

std::string mod_text;

void
SK::D3D9::TextureManager::updateQueueOSD (void)
{
  if (false)//true)//config.textures.show_loading_text)
  {
    DWORD dwTime = timeGetTime ();

    //if (TryEnterCriticalSection (&osd_cs))
    {
      LONG resample_count = ReadAcquire (&injector.resampling); size_t queue_len = resample_pool->queueLength ();
      LONG stream_count   = ReadAcquire (&injector.streaming);  size_t to_stream = injector.textures_to_stream.size ();

      bool is_resampling = (resample_pool->working () || resample_count || queue_len);
      bool is_streaming  = (stream_pool.working    () || stream_count   || to_stream);

      static std::string resampling_text; static DWORD dwLastResample = 0;
      static std::string streaming_text;  static DWORD dwLastStream   = 0;
      
      if (is_resampling)
      {
        size_t count = queue_len + resample_count;

            char szFormatted [64];
        sprintf (szFormatted, "  Resampling: %zu texture", count);

        resampling_text  = szFormatted;
        resampling_text += (count != 1) ? 's' : ' ';

        if (queue_len)
        {
          sprintf (szFormatted, " (%zu queued)", queue_len);
          resampling_text += szFormatted;
        }

        resampling_text += "\n";

        if (count)
          dwLastResample = dwTime;
      }
      
      if (is_streaming)
      {
        size_t count = stream_count + to_stream;

            char szFormatted [64];
        sprintf (szFormatted, "  Streaming:  %zu texture", count);

        streaming_text  = szFormatted;
        streaming_text += (count != 1) ? 's' : ' ';

        sprintf (szFormatted, " [%7.2f MiB]", (double)ReadAcquire ((volatile LONG *)&injector.streaming_bytes) / (1024.0f * 1024.0f));
        streaming_text += szFormatted;

        if (to_stream)
        {
          sprintf (szFormatted, " (%zu queued)", to_stream);
          streaming_text += szFormatted;
        }

        if (count)
          dwLastStream = dwTime;
      }

      if (dwLastResample < dwTime - 150)
        resampling_text = "";

      if (dwLastStream < dwTime - 150)
        streaming_text = "";

      mod_text = resampling_text + streaming_text;

      if (mod_text != "")
        last_queue_update = dwTime;
      
      //LeaveCriticalSection (&osd_cs);
    }
  }
}

int
SK::D3D9::TextureManager::loadQueuedTextures (void)
{
  updateQueueOSD ();

  int loads = 0;

  std::vector <TexLoadRequest *> finished_resamples;
  std::vector <TexLoadRequest *> finished_streams;

  stream_pool.getFinished (finished_streams);

  if (resample_pool != nullptr)
    resample_pool->getFinished (finished_resamples);

  for ( auto it : finished_resamples )
  {
    TexLoadRequest* load =
      it;

    QueryPerformanceCounter_Original (&load->end);

    if (true)
    {
      tex_log.Log ( L"[%s] Finished %s texture %08x (%5.2f MiB in %9.4f ms)",
                      (load->type == TexLoadRequest::Stream) ? L"Inject Tex" :
                        (load->type == TexLoadRequest::Immediate) ? L"Inject Tex" :
                                                                    L" Resample ",
                      (load->type == TexLoadRequest::Stream) ? L"streaming" :
                        (load->type == TexLoadRequest::Immediate) ? L"loading" :
                                                                    L"filtering",
                        load->checksum,
                          (double)load->SrcDataSize / (1024.0f * 1024.0f),
                            1000.0f * (double)(load->end.QuadPart - load->start.QuadPart) /
                                      (double)SK_GetPerfFreq ().QuadPart );
    }

    Texture* pTex =
      getTexture (load->checksum);

    if (pTex != nullptr)
    {
      pTex->load_time = (float)(1000.0 * (double)(load->end.QuadPart - load->start.QuadPart) /
                                          (double)SK_GetPerfFreq ().QuadPart);
    }

    auto* pSKTex =
      static_cast <ISKTextureD3D9 *> (load->pDest);

    if (pSKTex != nullptr)
    {
      if (pSKTex->refs == 0 && load->pSrc != nullptr)
      {
        tex_log.Log (L"[ Tex. Mgr ] >> Original texture no longer referenced, discarding new one!");
        load->pSrc->Release ();
      }

      else
      {
        QueryPerformanceCounter_Original (&pSKTex->last_used);

        pSKTex->pTexOverride  = load->pSrc;
        pSKTex->override_size = load->SrcDataSize;

        addInjected (load->SrcDataSize);
      }

      injector.finishedStreaming (load->checksum);

      updateOSD ();

      ++loads;

      // Remove the temporary reference
      load->pDest->Release ();
    }

    delete load;
  }

  int max_per_frame = 128;
  int loaded        = 0;

  static int frame = 0;

  frame++;

  for ( auto it : finished_streams )
  {
    if ((! __need_purge) && (loaded > max_per_frame || (frame % 2) == 0))
    {
      it->pDest->AddRef ();
      stream_pool.lrg_tex->postFinished (it);
      continue;
    }

    TexLoadRequest* load =
      it;

    QueryPerformanceCounter_Original (&load->end);

    if (true)
    {
      tex_log.Log ( L"[%s] Finished %s texture %08x (%5.2f MiB in %9.4f ms)",
                      (load->type == TexLoadRequest::Stream) ? L"Inject Tex" :
                        (load->type == TexLoadRequest::Immediate) ? L"Inject Tex" :
                                                                    L" Resample ",
                      (load->type == TexLoadRequest::Stream) ? L"streaming" :
                        (load->type == TexLoadRequest::Immediate) ? L"loading" :
                                                                    L"filtering",
                        load->checksum,
                          (double)load->SrcDataSize / (1024.0f * 1024.0f),
                            1000.0f * (double)(load->end.QuadPart - load->start.QuadPart) /
                                      (double)SK_GetPerfFreq ().QuadPart );
    }

    Texture* pTex =
      getTexture (load->checksum);

    if (pTex != nullptr)
    {
      pTex->load_time = (float)(1000.0 * (double)(load->end.QuadPart - load->start.QuadPart) /
                                         (double)SK_GetPerfFreq ().QuadPart);
    }

    auto* pSKTex =
      static_cast <ISKTextureD3D9 *> (load->pDest);

    if (pSKTex != nullptr)
    {
      if (pSKTex->refs == 0 && load->pSrc != nullptr)
      {
        tex_log.Log (L"[ Tex. Mgr ] >> Original texture no longer referenced, discarding new one!");
        load->pSrc->Release ();
      }

      else
      {
        QueryPerformanceCounter_Original (&pSKTex->last_used);

        pSKTex->pTexOverride  = load->pSrc;
        pSKTex->override_size = load->SrcDataSize;

        addInjected (load->SrcDataSize);

        injected_textures   [pSKTex->tex_crc32c] = pSKTex->pTexOverride;
        injected_sizes      [pSKTex->tex_crc32c] = pSKTex->override_size;
        injected_load_times [pSKTex->tex_crc32c] = pTex->load_time;
      }

      injector.finishedStreaming (load->checksum);

      updateOSD ();

      ++loads;

      // Remove the temporary reference
      load->pDest->Release ();

      //++loaded;
    }

    delete load;
  }

  //
  // If the size changes, check to see if we need a purge - if so, schedule one.
  //
  static int64_t last_size = 0LL;

  if (last_size != cacheSizeTotal ())
  {
    last_size = cacheSizeTotal ();

    if ( last_size >
           (1024LL * 1024ULL) * (int64_t)d3d9_max_cache_in_mib/*config.textures.max_cache_in_mib*/ )
      __need_purge = true;
  }

  if ( (! ReadAcquire (&injector.streaming))  &&
       (! ReadAcquire (&injector.resampling)) &&
       (! injector.hasPendingLoads ()) )
  {
    if (__need_purge)
    {
      purge ();
      __need_purge = false;
    }
  }

  osdStats ();

  return loads;
}

#include <set>


 static bool TOS = ( GetModuleHandle  (L"TOS.exe") != nullptr );

uint32_t
safe_crc32c (uint32_t seed, const void* pData, size_t size)
{
  __try
  {
    if (TOS)
      return crc32 (seed, pData, size);

    return crc32c (seed, pData, size);
  }

  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return 0x00;
  }
}

std::set <uint32_t> resample_blacklist;
bool                resample_blacklist_init = false;

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3DXCreateTextureFromFileInMemoryEx_Detour (
  _In_    LPDIRECT3DDEVICE9  pDevice,
  _In_    LPCVOID            pSrcData,
  _In_    UINT               SrcDataSize,
  _In_    UINT               Width,
  _In_    UINT               Height,
  _In_    UINT               MipLevels,
  _In_    DWORD              Usage,
  _In_    D3DFORMAT          Format,
  _In_    D3DPOOL            Pool,
  _In_    DWORD              Filter,
  _In_    DWORD              MipFilter,
  _In_    D3DCOLOR           ColorKey,
  _Inout_ D3DXIMAGE_INFO     *pSrcInfo,
  _Out_   PALETTEENTRY       *pPalette,
  _Out_   LPDIRECT3DTEXTURE9 *ppTexture
)
{
  // Injection would recurse slightly and cause impossible to diagnose reference counting problems
  //   with texture caching if we did not check for this!
  if (SK::D3D9::tex_mgr.injector.isInjectionThread ())
  {
    return
      D3DXCreateTextureFromFileInMemoryEx_Original (
        pDevice,
          pSrcData, SrcDataSize,
            Width, Height, MipLevels,
              Usage,
                Format,
                  Pool,
                    Filter, MipFilter, ColorKey,
                      pSrcInfo, pPalette,
                        ppTexture
      );
  }

  if (resample_blacklist_init == false)
  {
    resample_blacklist_init = true;
  }

  // Performance statistics for caching system
  LARGE_INTEGER start, end;

  QueryPerformanceCounter_Original (&start);

  uint32_t checksum = 0xdeadbeef;

  // Don't dump or cache these

  
  if ( (Usage & D3DUSAGE_DYNAMIC) || (Usage & D3DUSAGE_RENDERTARGET) || pSrcData == nullptr || SrcDataSize == 0 )
    checksum = 0x00;
  else
    checksum = safe_crc32c (0, pSrcData, SrcDataSize);

  if (true/*config.textures.d3d11.cache*/ && checksum != 0x00)
  {
    Texture* pTex =
      tex_mgr.getTexture (checksum);

    if (pTex != nullptr)
    {
      tex_mgr.refTexture (pTex);

      *ppTexture = pTex->d3d9_tex;

      return S_OK;
    }

    tex_mgr.missTexture ();
  }

  bool resample = false;


  // Necessary to make D3DX texture write functions work
  if ( Pool == D3DPOOL_DEFAULT && ( config.textures.dump_on_load &&
        (! tex_mgr.isTextureDumped     (checksum))         &&
        (! tex_mgr.isTextureInjectable (checksum)) ) || (
                                    config.textures.on_demand_dump ) )
    Usage = D3DUSAGE_DYNAMIC;



  tex_mgr.injector.beginLoad  ();

  D3DXIMAGE_INFO info = { };
  D3DXGetImageInfoFromFileInMemory (pSrcData, SrcDataSize, &info);

  tex_mgr.injector.endLoad  ();

#if 0
  D3DFORMAT fmt_real = info.Format;

  bool power_of_two_in_one_way =  
    (! (info.Width  & (info.Width  - 1)))  !=  (! (info.Height & (info.Height - 1)));


  // Textures that would be incorrectly filtered if resampled
  if (power_of_two_in_one_way)
    tex_mgr.addNonPowerOfTwoTexture (checksum);


  // Generate complete mipmap chains for best image quality
  //  (will increase load-time on uncached textures)
  if ((Pool == D3DPOOL_DEFAULT) && false)//config.textures.remaster)
  {
    {
      bool power_of_two_in =
        (! (info.Width  & (info.Width  - 1)))  &&  (! (info.Height & (info.Height - 1)));

      bool power_of_two_out =
        (! (Width  & (Width  - 1)))            &&  (! (Height & (Height - 1)));

      if (power_of_two_in && power_of_two_out)
      {
        if (true)//info.MipLevels > 1/* || config.textures.uncompressed*/)
        {
          if ( resample_blacklist.count (checksum) == 0 )
            resample = true;
        }
      }
    }
  }
#endif

  HRESULT         hr           = E_FAIL;
  TexLoadRequest* load_op      = nullptr;

  wchar_t wszInjectFileName [MAX_PATH] = { L'\0' };

  bool remap_stream =
    tex_mgr.injector.isStreaming (checksum);

  //
  // Generic injectable textures
  //
  if ( tex_mgr.isTextureInjectable (checksum) )
  {
    tex_log.LogEx ( true, L"[Inject Tex] Injectable texture for checksum (%08x)... ",
                      checksum );

    TexRecord& record =
      tex_mgr.getInjectableTexture (checksum);

    if (record.method == TexLoadMethod::DontCare)
      record.method = TexLoadMethod::Streaming;

    // If -1, load from disk...
    if (record.archive == -1)
    {
      if (record.method == TexLoadMethod::Streaming)
      {
        _swprintf ( wszInjectFileName, L"%s\\inject\\textures\\streaming\\%08x%s",
                      SK_D3D11_res_root.c_str (),
                        checksum,
                          L".dds" );
      }

      else if (record.method == TexLoadMethod::Blocking)
      {
        _swprintf ( wszInjectFileName, L"%s\\inject\\textures\\blocking\\%08x%s",
                      SK_D3D11_res_root.c_str (),
                        checksum,
                          L".dds" );
      }
    }

    load_op           = new TexLoadRequest ();

    load_op->pDevice  = pDevice;
    load_op->checksum = checksum;

    if (record.method == TexLoadMethod::Streaming)
      load_op->type   = TexLoadRequest::Stream;
    else
      load_op->type   = TexLoadRequest::Immediate;

    wcscpy (load_op->wszFilename, wszInjectFileName);

    if (load_op->type == TexLoadRequest::Stream)
    {
      if ((! remap_stream))
        tex_log.LogEx ( false, L"streaming\n" );
      else
        tex_log.LogEx ( false, L"in-flight already\n" );
    }

    else
    {
      tex_log.LogEx ( false, L"blocking (deferred)\n" );
    }
  }

  bool will_replace = (load_op != nullptr || resample);


  if (checksum != 0x00)
    tex_mgr.injector.beginLoad ();

  //tex_log->Log (L"D3DXCreateTextureFromFileInMemoryEx (... MipLevels=%lu ...)", MipLevels);
  hr =
    D3DXCreateTextureFromFileInMemoryEx_Original ( pDevice,
                                                     pSrcData,         SrcDataSize,
                                                       Width,          Height,    will_replace ? 1 : MipLevels,
                                                         Usage,        Format,    Pool,
                                                           Filter,     MipFilter, ColorKey,
                                                             pSrcInfo, pPalette,
                                                               ppTexture );

  if (checksum != 0x00)
    tex_mgr.injector.endLoad ();


  if (SUCCEEDED (hr))
  {
    new ISKTextureD3D9 (ppTexture, SrcDataSize, checksum);

    if ( load_op != nullptr && ( load_op->type == TexLoadRequest::Stream ||
                                 load_op->type == TexLoadRequest::Immediate ) )
    {
      load_op->SrcDataSize =
        static_cast <UINT> (
          tex_mgr.isTextureInjectable (checksum)         ?
            tex_mgr.getInjectableTexture (checksum).size : 0
        );

      load_op->pDest =
        *ppTexture;

      if (load_op->type == TexLoadRequest::Immediate)
        ((ISKTextureD3D9 *)*ppTexture)->must_block = true;

      if (tex_mgr.injector.isStreaming (load_op->checksum))
      {
        tex_mgr.injector.lockStreaming ();

        auto* pTexOrig =
          static_cast <ISKTextureD3D9 *> (
            tex_mgr.injector.getTextureInFlight (load_op->checksum)->pDest
          );

        tex_mgr.injector.unlockStreaming ();
        tex_mgr.injector.lockStreaming   ();

        // Remap the output of the in-flight texture
        tex_mgr.injector.getTextureInFlight (load_op->checksum)->pDest =
          *ppTexture;

        tex_mgr.injector.unlockStreaming ();

        Texture* pTex =
          tex_mgr.getTexture (load_op->checksum);

        if (pTex != nullptr)
        {
          for ( int i = 0;
                    i < pTex->refs;
                  ++i )
          {
            (*ppTexture)->AddRef ();
          }
        }

        tex_mgr.removeTexture (pTexOrig);
      }

      else
      {
        tex_mgr.injector.addTextureInFlight (load_op);
        stream_pool.postJob                 (load_op);
        //resample_pool->postJob (load_op);
      }
    }

#if 0
    //
    // TODO:  Actually stream these, but block if the game tries to call SetTexture (...)
    //          while the texture is in-flight.
    //
    else if (load_op != nullptr && load_op->type == tsf_tex_load_s::Immediate) {
      QueryPerformanceFrequency        (&load_op->freq);
      QueryPerformanceCounter_Original (&load_op->start);

      EnterCriticalSection (&cs_tex_inject);
      inject_tids.insert   (GetCurrentThreadId ());
      LeaveCriticalSection (&cs_tex_inject);

      load_op->pDest = *ppTexture;

      hr = InjectTexture (load_op);

      EnterCriticalSection (&cs_tex_inject);
      inject_tids.erase    (GetCurrentThreadId ());
      LeaveCriticalSection (&cs_tex_inject);

      QueryPerformanceCounter_Original (&load_op->end);

      if (SUCCEEDED (hr)) {
        tex_log->Log ( L"[Inject Tex] Finished synchronous texture %08x (%5.2f MiB in %9.4f ms)",
                        load_op->checksum,
                          (double)load_op->SrcDataSize / (1024.0f * 1024.0f),
                            1000.0f * (double)(load_op->end.QuadPart - load_op->start.QuadPart) /
                                      (double) load_op->freq.QuadPart );
        ISKTextureD3D9* pSKTex =
          (ISKTextureD3D9 *)*ppTexture;

        pSKTex->pTexOverride  = load_op->pSrc;
        pSKTex->override_size = load_op->SrcDataSize;

        pSKTex->last_used     = load_op->end;

        tsf::RenderFix::tex_mgr.addInjected (load_op->SrcDataSize);
      } else {
        tex_log->Log ( L"[Inject Tex] *** FAILED synchronous texture %08x",
                        load_op->checksum );
      }

      delete load_op;
      load_op = nullptr;
    }
#endif

    else if (resample)
    {
      load_op              = new TexLoadRequest ();

      load_op->pDevice     = pDevice;
      load_op->checksum    = checksum;
      load_op->type        = TexLoadRequest::Resample;

      load_op->pSrcData    = new uint8_t [SrcDataSize];
      load_op->SrcDataSize = SrcDataSize;

      swprintf (load_op->wszFilename, L"Resample_%x.dds", checksum);

      memcpy (load_op->pSrcData, pSrcData, SrcDataSize);

      (*ppTexture)->AddRef ();
      load_op->pDest       = *ppTexture;

      resample_pool->postJob (load_op);
    }
  }

  else if (load_op != nullptr)
  {
    delete load_op;
    load_op = nullptr;
  }

  QueryPerformanceCounter_Original (&end);

  if (SUCCEEDED (hr))
  {
    if (/*config.textures.cache &&*/ checksum != 0x00)
    {
      auto* pTex =
        new Texture ();

      pTex->crc32c = checksum;

      pTex->d3d9_tex = *(ISKTextureD3D9 **)ppTexture;
      pTex->d3d9_tex->AddRef ();
      pTex->refs++;

      pTex->load_time = (float)( 1000.0 *
                          (double)(end.QuadPart - start.QuadPart) /
                          (double)SK_GetPerfFreq ().QuadPart );

      tex_mgr.addTexture (checksum, pTex, SrcDataSize);
    }

    if (true)
    {//config.textures.log) {
      tex_log.Log ( L"[Load Trace] Texture:   (%lu x %lu) * <LODs: %lu> - FAST_CRC32: %X",
                     info.Width, info.Height, (*ppTexture)->GetLevelCount (), checksum );
      tex_log.Log ( L"[Load Trace]              Usage: %-20s - Format: %-20s",
                     SK_D3D9_UsageToStr    (Usage).c_str (),
                       SK_D3D9_FormatToStr (Format).c_str () );
      tex_log.Log ( L"[Load Trace]                Pool: %s",
                     SK_D3D9_PoolToStr (Pool) );
      tex_log.Log ( L"[Load Trace]      Load Time: %6.4f ms", 
                     1000.0f * (double)(end.QuadPart - start.QuadPart) / (double)SK_GetPerfFreq ().QuadPart );
    }
  }

  bool dump = config.textures.dump_on_load;

  if ( dump && (! tex_mgr.isTextureInjectable (checksum)) &&
               (! tex_mgr.isTextureDumped     (checksum)) )
  {
    D3DXIMAGE_INFO info_     = {            };
    D3DFORMAT      fmt_real_ = D3DFMT_UNKNOWN;

    tex_mgr.injector.beginLoad  ();

    D3DXGetImageInfoFromFileInMemory (pSrcData, SrcDataSize, &info_);

    fmt_real_ = info_.Format;

    tex_mgr.dumpTexture (fmt_real_, checksum, *ppTexture);

    tex_mgr.injector.endLoad  ();
  }

  return hr;
}

bool
SK::D3D9::TextureManager::deleteDumpedTexture (D3DFORMAT fmt, uint32_t checksum)
{
  wchar_t wszPath [MAX_PATH];
  _swprintf ( wszPath, L"%s\\dump",
                SK_D3D11_res_root.c_str () );

  if (GetFileAttributesW (wszPath) != FILE_ATTRIBUTE_DIRECTORY)
    CreateDirectoryW (wszPath, nullptr);

  _swprintf ( wszPath, L"%s\\dump\\textures",
                SK_D3D11_res_root.c_str () );

  if (GetFileAttributesW (wszPath) != FILE_ATTRIBUTE_DIRECTORY)
    CreateDirectoryW (wszPath, nullptr);

  _swprintf ( wszPath, L"%s\\%s",
                wszPath,
                  SK_D3D9_FormatToStr (fmt, false).c_str () );

  if (GetFileAttributesW (wszPath) != FILE_ATTRIBUTE_DIRECTORY)
    CreateDirectoryW (wszPath, nullptr);

  wchar_t wszFileName [MAX_PATH] = { L'\0' };
  _swprintf ( wszFileName, L"%s\\dump\\textures\\%s\\%08x%s",
                SK_D3D11_res_root.c_str (),
                  SK_D3D9_FormatToStr (fmt, false).c_str (),
                    checksum,
                      L".dds" );

  if (GetFileAttributesW (wszFileName) != INVALID_FILE_ATTRIBUTES)
  {
    if (DeleteFileW (wszFileName))
    {
      // TODO: Add critical section to guard this
      dumped_textures.erase (checksum);
      return true;
    }
  }

  return false;
}

bool
SK::D3D9::TextureManager::isTextureDumped (uint32_t checksum)
{
  return dumped_textures.contains (checksum);
}

HRESULT
SK::D3D9::TextureManager::dumpTexture (D3DFORMAT fmt, uint32_t checksum, IDirect3DTexture9* pTex)
{
  if ( (! isTextureInjectable (checksum)) &&
       (! isTextureDumped     (checksum)) )
  {
    injector.lockDumping ();

    D3DFORMAT fmt_real = fmt;

//    bool compressed =
//      (fmt_real >= D3DFMT_DXT1 && fmt_real <= D3DFMT_DXT5);

    wchar_t wszPath [MAX_PATH];
    _swprintf ( wszPath, L"%s\\dump",
                  SK_D3D11_res_root.c_str () );

    if (GetFileAttributesW (wszPath) != FILE_ATTRIBUTE_DIRECTORY)
      CreateDirectoryW (wszPath, nullptr);

    _swprintf ( wszPath, L"%s\\dump\\textures",
                  SK_D3D11_res_root.c_str () );

    if (GetFileAttributesW (wszPath) != FILE_ATTRIBUTE_DIRECTORY)
      CreateDirectoryW (wszPath, nullptr);

    _swprintf ( wszPath, L"%s\\%s",
                  wszPath,
                   SK_D3D9_FormatToStr (fmt_real, false).c_str () );

    if (GetFileAttributesW (wszPath) != FILE_ATTRIBUTE_DIRECTORY)
      CreateDirectoryW (wszPath, nullptr);

    wchar_t wszFileName [MAX_PATH] = { L'\0' };
    _swprintf ( wszFileName, L"%s\\dump\\textures\\%s\\%08x%s",
                  SK_D3D11_res_root.c_str (),
                    SK_D3D9_FormatToStr (fmt_real, false).c_str (),
                      checksum,
                        L".dds" );

    injector.beginLoad ();

    HRESULT hr =
      D3DXSaveTextureToFile (wszFileName, D3DXIFF_DDS, pTex, nullptr);

    injector.endLoad ();

    if (SUCCEEDED (hr))
      dumped_textures.emplace (checksum);

    injector.unlockDumping ();

    return hr;
  }

  return E_FAIL;
}

std::vector <ISKTextureD3D9 *> remove_textures;

SK::D3D9::Texture*
SK::D3D9::TextureManager::getTexture (uint32_t checksum)
{
  if (checksum == 0x00)
  {
  std::vector <ISKTextureD3D9 *> unremove_textures;
  EnterCriticalSection (&cs_free_list);

    auto  rem = remove_textures.begin ();

    while (rem != remove_textures.end ())
    {
      if (! (*rem)->can_free)
      {
        unremove_textures.emplace_back (*rem);
        ++rem;
        continue;
      }

      if ((*rem)->pTexOverride != nullptr)
      {
        InterlockedDecrement (&injected_count);
        InterlockedAdd64     (&injected_size, -(*rem)->override_size);
      }

      if ((*rem)->pTex)         (*rem)->pTex->Release         ();
      //if ((*rem)->pTexOverride) (*rem)->pTexOverride->Release ();

      (*rem)->pTex         = nullptr;
      (*rem)->pTexOverride = nullptr;

      InterlockedAdd64 (&basic_size,  -(*rem)->tex_size);
      {
        textures.erase          ((*rem)->tex_crc32c);
      }

      //delete *rem;

      ++rem;
    }

    remove_textures = unremove_textures;
  LeaveCriticalSection (&cs_free_list);
  }


  EnterCriticalSection (&cs_cache);

    auto tex = textures.find (checksum);

    if (tex != textures.end ())
    {
      LeaveCriticalSection (&cs_cache);
      return tex->second;
    }

  LeaveCriticalSection (&cs_cache);

  return nullptr;
}

void
SK::D3D9::TextureManager::removeTexture (ISKTextureD3D9* pTexD3D9)
{
  EnterCriticalSection (&cs_free_list);

    remove_textures.emplace_back (pTexD3D9);

  LeaveCriticalSection (&cs_free_list);

  updateOSD ();
}

void
SK::D3D9::TextureManager::addTexture (uint32_t checksum, Texture* pTex, size_t size)
{
  pTex->size = size;

  InterlockedAdd64 (&basic_size, pTex->size);

  EnterCriticalSection (&cs_cache);
  {
    if (textures.count (checksum))
    {
      if (textures [checksum] != pTex)
      {
        removeTexture (textures [checksum]->d3d9_tex);
      }
    }

    textures [checksum] = pTex;
  }
  LeaveCriticalSection (&cs_cache);

  updateOSD ();
}

void
SK::D3D9::TextureManager::refTexture (Texture* pTex)
{
  refTextureEx (pTex, true);
}

void
SK::D3D9::TextureManager::refTextureEx (Texture* pTex, bool add_to_ref_count)
{
  if (add_to_ref_count)
  {
    pTex->d3d9_tex->AddRef ();
    pTex->refs++;
  }

  pTex->d3d9_tex->can_free = false;

  InterlockedIncrement (&hits);

#if 0
  if (true) 
  {//config.textures.log) {
    tex_log.Log ( L"[CacheTrace] Cache hit (%X), saved %2.1f ms",
                    pTex->crc32c,
                      pTex->load_time );
  }
#endif

  InterlockedAdd64 ( &bytes_saved,
                       std::max ( pTex->size, static_cast <size_t> (
                                                pTex->d3d9_tex->override_size )
                                )
                    );
                    time_saved += pTex->load_time;

  updateOSD ();
}

COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9SetRenderTarget_Detour (
                  _In_ IDirect3DDevice9  *This,
                  _In_ DWORD              RenderTargetIndex,
                  _In_ IDirect3DSurface9 *pRenderTarget
)
{
  static int draw_counter = 0;

  // Ignore anything that's not the primary render device.
  if (This != SK_GetCurrentRenderBackend ().device)
  {
    return D3D9SetRenderTarget_Original (This, RenderTargetIndex, pRenderTarget);
  }

  //if (tsf::RenderFix::tracer.log) {
#ifdef DUMP_RT
    if (D3DXSaveSurfaceToFileW == nullptr) {
      D3DXSaveSurfaceToFileW =
        (D3DXSaveSurfaceToFile_pfn)
          GetProcAddress ( tsf::RenderFix::d3dx9_43_dll,
                             "D3DXSaveSurfaceToFileW" );
    }

    wchar_t wszDumpName [MAX_PATH];

    if (pRenderTarget != pOld) {
      if (pOld != nullptr) {
        wsprintf (wszDumpName, L"dump\\%03d_out_%p.png", draw_counter, pOld);

        dll_log->Log ( L"[FrameTrace] >>> Dumped: Output RT to %s >>>", wszDumpName );

        dumping = true;
        //D3DXSaveSurfaceToFile (wszDumpName, D3DXIFF_PNG, pOld, nullptr, nullptr);
      }
    }
#endif

    //dll_log->Log ( L"[FrameTrace] SetRenderTarget - RenderTargetIndex: %lu, pRenderTarget: %ph",
                    //RenderTargetIndex, pRenderTarget );

#ifdef DUMP_RT
    if (pRenderTarget != pOld) {
      pOld = pRenderTarget;

      wsprintf (wszDumpName, L"dump\\%03d_in_%p.png", ++draw_counter, pRenderTarget);

      dll_log->Log ( L"[FrameTrace] <<< Dumped: Input RT to  %s  <<<", wszDumpName );

      dumping = true;
      //D3DXSaveSurfaceToFile (wszDumpName, D3DXIFF_PNG, pRenderTarget, nullptr, nullptr);
    }
#endif
  //}

  return D3D9SetRenderTarget_Original (This, RenderTargetIndex, pRenderTarget);
}

HMODULE d3dx9_43_dll;

void
SK::D3D9::TextureManager::Init (void)
{
  textures.reserve                    (4096);
  textures_used.reserve               (2048);
  textures_last_frame.reserve         (1024);
//non_power_of_two_textures.reserve   (512);
  tracked_ps.used_textures.reserve    (512);
  tracked_vs.used_textures.reserve    (512);
  known.render_targets.reserve        (128);
  used.render_targets.reserve         (64);
  injector.textures_in_flight.reserve (32);
  tracked_rt.pixel_shaders.reserve    (32);
  tracked_rt.vertex_shaders.reserve   (32);

  InitializeCriticalSectionAndSpinCount (&cs_cache,        10240UL);
  InitializeCriticalSectionAndSpinCount (&cs_free_list,     6144UL);
  InitializeCriticalSectionAndSpinCount (&cs_unreferenced, 16384UL);

  InitializeCriticalSectionAndSpinCount (&osd_cs,   32UL);

  InitializeCriticalSectionAndSpinCount (&injector.cs_tex_inject,    1000000);
  InitializeCriticalSectionAndSpinCount (&injector.cs_tex_blacklist, 1000000);
  InitializeCriticalSectionAndSpinCount (&injector.cs_tex_resample,  10000);
  InitializeCriticalSectionAndSpinCount (&injector.cs_tex_stream,    10000);
  InitializeCriticalSectionAndSpinCount (&injector.cs_tex_dump,      1000);

  void WINAPI SK_D3D11_SetResourceRoot      (const wchar_t* root);
  SK_D3D11_SetResourceRoot (config.textures.d3d11.res_root.c_str ());

  // Create the directory to store dumped textures
  if (config.textures.d3d11.dump)
    CreateDirectoryW (SK_D3D11_res_root.c_str (), nullptr);

  tex_log.init (L"logs/textures.log", L"w+");

  d3dx9_43_dll =
    LoadLibraryW (L"D3DX9_43.DLL");

  init = true;

  refreshDataSources ();

  if ( GetFileAttributesW ((SK_D3D11_res_root + L"\\dump\\textures").c_str ()) !=
         INVALID_FILE_ATTRIBUTES )
  {
    WIN32_FIND_DATA fd;
    WIN32_FIND_DATA fd_sub;
    HANDLE          hSubFind = INVALID_HANDLE_VALUE;
    HANDLE          hFind    = INVALID_HANDLE_VALUE;
    int             files    = 0;
    LARGE_INTEGER   liSize   = { 0 };

    tex_log.LogEx ( true, L"[ Dump Tex ] Enumerating dumped textures..." );

    hFind =
      FindFirstFileW ((SK_D3D11_res_root + L"\\dump\\textures\\*").c_str (), &fd);

    if (hFind != INVALID_HANDLE_VALUE)
    {
      do
      {
        if (fd.dwFileAttributes != INVALID_FILE_ATTRIBUTES)
        {
          wchar_t wszSubDir [MAX_PATH] = { };
          _swprintf (wszSubDir, L"%s\\dump\\textures\\%s\\*", SK_D3D11_res_root.c_str (), fd.cFileName);

          hSubFind =
            FindFirstFileW (wszSubDir, &fd_sub);

          if (hSubFind != INVALID_HANDLE_VALUE)
          {
            do
            {
              if (wcsstr (_wcslwr (fd_sub.cFileName), L".dds"))
              {
                uint32_t checksum = 0x00;
                swscanf (fd_sub.cFileName, L"%08x.dds", &checksum);

                ++files;

                LARGE_INTEGER fsize;

                fsize.HighPart = fd_sub.nFileSizeHigh;
                fsize.LowPart  = fd_sub.nFileSizeLow;

                liSize.QuadPart += fsize.QuadPart;

                dumped_textures.emplace (checksum);
              }
            } while (FindNextFileW (hSubFind, &fd_sub) != 0);

            FindClose (hSubFind);
          }
        }
      } while (FindNextFileW (hFind, &fd) != 0);

      FindClose (hFind);
    }

    tex_log.LogEx ( false, L" %lu files (%3.1f MiB)\n",
                      files, (double)liSize.QuadPart / (1024.0 * 1024.0) );
  }

  InterlockedExchange64 (&bytes_saved, 0LL);

  time_saved  = 0.0f;

  decomp_semaphore = 
    CreateSemaphore ( nullptr,
                        2,//config.textures.worker_threads,
                          2,//config.textures.worker_threads,
                            nullptr );

  resample_pool       = new TextureThreadPool ();

  stream_pool.lrg_tex = new TextureThreadPool ();
  stream_pool.sm_tex  = new TextureThreadPool ();

  SK_ICommandProcessor& command =
    *SK_GetCommandProcessor ();

  command.AddVariable (
    "Textures.Remap",
      SK_CreateVar (SK_IVariable::Boolean, &__remap_textures) );

  command.AddVariable (
    "Textures.Purge",
      SK_CreateVar (SK_IVariable::Boolean, &__need_purge) );

  command.AddVariable (
    "Textures.Trace",
      SK_CreateVar (SK_IVariable::Boolean, &__log_used) );

  command.AddVariable (
    "Textures.ShowCache",
      SK_CreateVar (SK_IVariable::Boolean, &__show_cache) );

  command.AddVariable (
    "Textures.MaxCacheSize",
      SK_CreateVar (SK_IVariable::Int,     &d3d9_max_cache_in_mib) );//&config.textures.max_cache_in_mib) );
}

#include <utility.h>

extern
COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9SetTexture_Override (
  _In_ IDirect3DDevice9      *This,
  _In_ DWORD                  Sampler,
  _In_ IDirect3DBaseTexture9 *pTexture );

extern
COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9SetRenderTarget_Override (
  _In_ IDirect3DDevice9  *This,
  _In_ DWORD              RenderTargetIndex,
  _In_ IDirect3DSurface9 *pRenderTarget );

extern
COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9CreateTexture_Override (IDirect3DDevice9   *This,
                            UINT                Width,
                            UINT                Height,
                            UINT                Levels,
                            DWORD               Usage,
                            D3DFORMAT           Format,
                            D3DPOOL             Pool,
                            IDirect3DTexture9 **ppTexture,
                            HANDLE             *pSharedHandle);

extern
COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9CreateDepthStencilSurface_Override (IDirect3DDevice9     *This,
                                        UINT                  Width,
                                        UINT                  Height,
                                        D3DFORMAT             Format,
                                        D3DMULTISAMPLE_TYPE   MultiSample,
                                        DWORD                 MultisampleQuality,
                                        BOOL                  Discard,
                                        IDirect3DSurface9   **ppSurface,
                                        HANDLE               *pSharedHandle);

extern
COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9CreateRenderTarget_Override (IDirect3DDevice9     *This,
                                 UINT                  Width,
                                 UINT                  Height,
                                 D3DFORMAT             Format,
                                 D3DMULTISAMPLE_TYPE   MultiSample,
                                 DWORD                 MultisampleQuality,
                                 BOOL                  Lockable,
                                 IDirect3DSurface9   **ppSurface,
                                 HANDLE               *pSharedHandle);

extern
COM_DECLSPEC_NOTHROW
HRESULT
STDMETHODCALLTYPE
D3D9SetDepthStencilSurface_Override (
  _In_ IDirect3DDevice9  *This,
  _In_ IDirect3DSurface9 *pNewZStencil
);

void
SK::D3D9::TextureManager::Hook (void)
{
  //SK_CreateDLLHook2 (      SK_GetModuleFullName (SK_GetDLL ()).c_str (),
  //                          "D3D9BeginScene_Override",
  //                           D3D9BeginScene_Detour,
  //  static_cast_p2p <void> (&D3D9BeginScene_Original) );
  //
  //SK_CreateDLLHook2 (      SK_GetModuleFullName (SK_GetDLL ()).c_str (),
  //                          "D3D9StretchRect_Override",
  //                           D3D9StretchRect_Detour,
  //  static_cast_p2p <void> (&D3D9StretchRect_Original) );
  //

  SK_CreateFuncHook (      L"D3D9CreateRenderTarget_Override",
                            &D3D9CreateRenderTarget_Override,
                            &D3D9CreateRenderTarget_Detour,
    static_cast_p2p <void> (&D3D9CreateRenderTarget_Original) );
  MH_QueueEnableHook (       D3D9CreateRenderTarget_Override );

  SK_CreateFuncHook (      L"D3D9CreateDepthStencilSurface_Override",
                            &D3D9CreateDepthStencilSurface_Override,
                            &D3D9CreateDepthStencilSurface_Detour,
    static_cast_p2p <void> (&D3D9CreateDepthStencilSurface_Original) );
  MH_QueueEnableHook (       D3D9CreateDepthStencilSurface_Override );

  SK_CreateFuncHook (      L"D3D9CreateTexture_Override",
                            &D3D9CreateTexture_Override,
                            &D3D9CreateTexture_Detour,
    static_cast_p2p <void> (&D3D9CreateTexture_Original) );
  MH_QueueEnableHook (       D3D9CreateTexture_Override );

  SK_CreateFuncHook (      L"D3D9SetTexture_Override",
                            &D3D9SetTexture_Override,
                            &D3D9SetTexture_Detour,
    static_cast_p2p <void> (&D3D9SetTexture_Original) );
  MH_QueueEnableHook (       D3D9SetTexture_Override );

  SK_CreateFuncHook (      L"D3D9SetRenderTarget_Override",
                            &D3D9SetRenderTarget_Override,
                            &D3D9SetRenderTarget_Detour,
    static_cast_p2p <void> (&D3D9SetRenderTarget_Original) );
  MH_QueueEnableHook (       D3D9SetRenderTarget_Override );

  //SK_CreateFuncHook ( L"D3D9SetDepthStencilSurface_Override",
  //                     &D3D9SetDepthStencilSurface_Override,
  //                     &D3D9SetDepthStencilSurface_Detour,
  //                     &D3D9SetDepthStencilSurface);
  //MH_QueueEnableHook (  D3D9SetDepthStencilSurface_Detour);

  SK_CreateDLLHook2 (      L"D3DX9_43.DLL",
                            "D3DXCreateTextureFromFileInMemoryEx",
                             D3DXCreateTextureFromFileInMemoryEx_Detour,
    static_cast_p2p <void> (&D3DXCreateTextureFromFileInMemoryEx_Original) );

  SK_ApplyQueuedHooks ();
}

// Skip the purge step on shutdown
bool shutting_down = false;

void
SK::D3D9::TextureManager::Shutdown (void)
{
  // 16.6 ms per-frame (60 FPS)
  const float frame_time = 16.6f;

  while (! injector.textures_to_stream.empty ())
    injector.textures_to_stream.pop ();

  shutting_down = true;

  tex_mgr.reset ();

  DeleteCriticalSection (&injector.cs_tex_stream);
  DeleteCriticalSection (&injector.cs_tex_resample);
  DeleteCriticalSection (&injector.cs_tex_inject);
  DeleteCriticalSection (&injector.cs_tex_blacklist);

  DeleteCriticalSection (&cs_cache);
  DeleteCriticalSection (&osd_cs);

  CloseHandle (decomp_semaphore);

  tex_log.Log ( L"[Perf Stats] At shutdown: %7.2f seconds (%7.2f frames)"
                L" saved by cache",
                  time_saved / 1000.0f,
                    time_saved / frame_time );
  tex_log.close ();

  while (! screenshots_to_delete.empty ())
  {
    std::wstring file_to_delete = screenshots_to_delete.front ();
    screenshots_to_delete.pop ();

    DeleteFileW (file_to_delete.c_str ());
  }

  FreeLibrary (d3dx9_43_dll);
}

void
SK::D3D9::TextureManager::purge (void)
{
  if (shutting_down)
    return;

  int      released           = 0;
  int      released_injected  = 0;
   int64_t reclaimed          = 0;
   int64_t reclaimed_injected = 0;

  tex_log.Log (L"[ Tex. Mgr ] -- TextureManager::purge (...) -- ");

  // Purge any pending removes
  getTexture (0);

  tex_log.Log ( L"[ Tex. Mgr ]  ***  Current Cache Size: %6.2f MiB "
                                          L"(User Limit: %6.2f MiB)",
                  (double)cacheSizeTotal () / (1024.0 * 1024.0),
                    (double)d3d9_max_cache_in_mib /*config.textures.max_cache_in_mib*/ );

  tex_log.Log (L"[ Tex. Mgr ]   Releasing textures...");

  EnterCriticalSection (&cs_unreferenced);
  EnterCriticalSection (&cs_cache);

  std::vector <Texture *> unreferenced_textures;

  for ( auto& it : textures )
  {
    if (it.second->d3d9_tex->can_free)
    {
      unreferenced_textures.emplace_back (it.second);
    }
  }
  LeaveCriticalSection (&cs_cache);

  std::sort ( unreferenced_textures.begin (),
                unreferenced_textures.end (),
      []( Texture *a,
          Texture *b )
    {
      return a->d3d9_tex->last_used.QuadPart <
             b->d3d9_tex->last_used.QuadPart;
    }
  );

  auto free_it =
    unreferenced_textures.begin ();

  // We need to over-free, or we will likely be purging every other texture load
  int64_t target_size =
    std::max (128, d3d9_max_cache_in_mib - 64) * 1024LL * 1024LL;
  int64_t start_size  =
    cacheSizeTotal ();

  while ( start_size - reclaimed > target_size &&
            free_it != unreferenced_textures.end () )
  {
    int             tex_refs = -1;
    ISKTextureD3D9* pSKTex   = (*free_it)->d3d9_tex;

    //
    // Skip loads that are in-flight so that we do not hitch
    //
    if (injector.isStreaming ((*free_it)->crc32c))
    {
      ++free_it;
      continue;
    }

    //
    // Do not evict blocking loads, they are generally small and
    //   will cause performance problems if we have to reload them
    //     again later.
    //
    if (pSKTex->must_block)
    {
      ++free_it;
      continue;
    }

    int64_t ovr_size  = 0;
    int64_t base_size = 0;

    ++free_it;

    base_size = pSKTex->tex_size;
    ovr_size  = pSKTex->override_size;
    tex_refs  = pSKTex->Release ();

    if (tex_refs == 0)
    {
      if (ovr_size != 0)
      {
        reclaimed += ovr_size;

        released_injected++;
        reclaimed_injected += ovr_size;
      }
    }

    else
    {
      tex_log.Log (L"[ Tex. Mgr ] Invalid reference count (%lu)!", tex_refs);
    }

    ++released;
    reclaimed  += base_size;
  }
  LeaveCriticalSection (&cs_unreferenced);

  tex_log.Log ( L"[ Tex. Mgr ]   %4d textures (%4zu remain)",
                  released,
                    textures.size () );

  tex_log.Log ( L"[ Tex. Mgr ]   >> Reclaimed %6.2f MiB of memory (%6.2f MiB from %lu inject)",
                  (double)reclaimed          / (1024.0 * 1024.0),
                  (double)reclaimed_injected / (1024.0 * 1024.0),
                          released_injected );

  updateOSD ();

  tex_log.Log (L"[ Tex. Mgr ] ----------- Finished ------------ ");
}

void
SK::D3D9::TextureManager::reset (void)
{
  if (! init)
    return;

  if (! outstanding_screenshots.empty ())
  {
    tex_log.LogEx (true, L"[Screenshot] A queued screenshot has not finished, delaying device reset...");

    while (! outstanding_screenshots.empty ())
      ;

    tex_log.LogEx (false, L"done!\n");
  }

  int iters = 0;

  while ((! injector.textures_in_flight.empty ()) || injector.hasPendingLoads ())
  {
    loadQueuedTextures ();
    SleepEx            (16, TRUE);
    getTexture         (0);

    if (++iters > 10)
      break;
  }

  known.render_targets.clear ();
  used.render_targets.clear  ();

  //int underflows       = 0;

  int ext_refs         = 0;
  int ext_textures     = 0;

  int release_count    = 0;
  int unreleased_count = 0;
  int ref_count        = 0;

  int      released_injected  = 0;
   int64_t reclaimed          = 0;
   int64_t reclaimed_injected = 0;

  tex_log.Log (L"[ Tex. Mgr ] -- TextureManager::reset (...) -- ");

  int64_t original_cache_size = cacheSizeTotal ();

  tex_log.Log (L"[ Tex. Mgr ]   Releasing textures...");

  for (auto& it : injected_textures)
  {
    it.second->Release ();
    reclaimed          += injected_sizes [it.first];
    released_injected++;
    reclaimed_injected += injected_sizes [it.first];
    injected_size      -= injected_sizes [it.first];
  }

  injected_textures.clear   ();
  injected_load_times.clear ();
  injected_sizes.clear      ();

  InterlockedExchange64 (&injected_size,  0);
  InterlockedExchange   (&injected_count, 0);

  for ( auto& it : textures )
  {
    ISKTextureD3D9* pSKTex =
      it.second->d3d9_tex;

    bool    can_free  = false;
    int64_t base_size = 0;
    int64_t ovr_size  = 0;

    int tex_refs =
      pSKTex->Release ();

    if (pSKTex->can_free)
    {
      can_free = true;
      base_size = pSKTex->tex_size;
      ovr_size  = pSKTex->override_size;
    }

    else
    {
      ext_refs     += pSKTex->refs;
      ext_textures ++;

      ++unreleased_count;
      continue;
    }

    if (tex_refs == 0 || pSKTex->can_free)
    {
      ++release_count;
      reclaimed += base_size;

      ref_count += 1;
    }

    else
    {
      ++unreleased_count;
      ext_refs     += tex_refs;
      ext_textures ++;
    }
  }

  getTexture (0);
  // Commit this immediately, such that D3D9 Reset will not fail in
  //   fullscreen mode...
  loadQueuedTextures ();

  tex_log.Log ( L"[ Tex. Mgr ]   %4d textures (%4d references)",
                  release_count + unreleased_count,
                      ref_count + ext_refs );

  if (ext_refs > 0)
  {
    tex_log.Log ( L"[ Tex. Mgr ] >> WARNING: The game is still holding references (%d) to %d textures !!!",
                    ext_refs, ext_textures );
  }

  if ((int32_t)cacheSizeBasic () < 0)
    InterlockedExchange64 (&basic_size, 0);

  tex_log.Log ( L"[ Mem. Mgr ] === Memory Management Summary ===");

  tex_log.Log ( L"[ Mem. Mgr ]  %12.2f MiB Freed",
                   (double)std::max (0LL, reclaimed) / (1048576.0) );
  tex_log.Log ( L"[ Mem. Mgr ]  %12.2f MiB Leaked",
                  (double)(original_cache_size - std::max (0LL, reclaimed))
                                             / (1048576.0) );

  updateOSD ();

  textures_used.clear           ();
  textures_last_frame.clear     ();

  tex_log.Log (L"[ Tex. Mgr ] ----------- Finished ------------ ");
}

void
SK::D3D9::TextureManager::updateOSD (void)
{
  return;

  if (! init)
    return;

  double cache_basic    = (double)cacheSizeBasic    () / (1048576.0f);
  double cache_injected = (double)cacheSizeInjected () / (1048576.0f);
  double cache_total    = cache_basic + cache_injected;

  osd_stats = "";

  char szFormatted [64];
  sprintf ( szFormatted, "%6zu Total Textures : %8.2f MiB",
              numTextures () + numInjectedTextures (),
                cache_total );
  osd_stats += szFormatted;

  CComPtr <IDirect3DDevice9> pDevice = nullptr;

  SK_RenderBackend& rb =
    SK_GetCurrentRenderBackend ();

  if (            rb.device != nullptr                                     &&
       SUCCEEDED (rb.device->QueryInterface <IDirect3DDevice9> (&pDevice)) &&
                    pDevice->GetAvailableTextureMem () / 1048576UL != 4095    )
  {
    sprintf ( szFormatted, "    (%4lu MiB Available)\n",
              pDevice->GetAvailableTextureMem () / 1048576UL );
  }
  else
    sprintf (szFormatted, "\n");

  osd_stats += szFormatted;

  sprintf ( szFormatted, "%6zu  Base Textures : %8.2f MiB    %s\n",
              numTextures (),
                cache_basic,
                  __remap_textures ? "" : "<----" );

  osd_stats += szFormatted;

  sprintf ( szFormatted, "%6lu   New Textures : %8.2f MiB    %s\n",
              numInjectedTextures (),
                cache_injected,
                  __remap_textures ? "<----" : "" );

  osd_stats += szFormatted;

  sprintf ( szFormatted, "%6lu Cache Hits     : %8.2f Seconds Saved",
              hits,
                time_saved / 1000.0f );

  osd_stats += szFormatted;

  if (debug_tex_id != 0x00)
  {
    osd_stats += "\n\n";

    sprintf ( szFormatted, " Debug Texture : %08x",
                debug_tex_id );

    osd_stats += szFormatted;
  }
}

std::vector <uint32_t> textures_used_last_dump;
              int32_t  tex_dbg_idx              = 0L;

void
SK::D3D9::TextureManager::logUsedTextures (void)
{
  if (! init)
    return;

  if (__log_used)
  {
    textures_used_last_dump.clear ();
    tex_dbg_idx = 0;

    tex_log.Log (L"[ Tex. Log ] ---------- FrameTrace ----------- ");

    for (const uint32_t it : textures_used)
    {
      auto tex_record =
        getTexture (it);

      // Handle the RARE case where a purge happens immediately following
      //   the last frame
      if ( tex_record           != nullptr &&
           tex_record->d3d9_tex != nullptr )
      {
        auto* pSKTex =
          static_cast <ISKTextureD3D9 *> (tex_record->d3d9_tex);

        textures_used_last_dump.emplace_back (it);

        tex_log.Log ( L"[ Tex. Log ] %08x.dds  { Base: %6.2f MiB,  "
                      L"Inject: %6.2f MiB,  Load Time: %8.3f ms }",
                        it,
                          (double)pSKTex->tex_size /
                            (1024.0 * 1024.0),

                    pSKTex->override_size != 0 ? 
                      (double)pSKTex->override_size / 
                            (1024.0 * 1024.0) : 0.0,

                          getTexture (it)->load_time );
      }
    }

    tex_log.Log (L"[ Tex. Log ] ---------- FrameTrace ----------- ");

    __log_used = false;
  }

  textures_used.clear ();
}


volatile LONG TextureWorkerThread::num_threads_init = 0UL;

HRESULT
WINAPI
ResampleTexture (TexLoadRequest* load)
{
  QueryPerformanceCounter   (&load->start);

  D3DXIMAGE_INFO img_info = { };

  D3DXGetImageInfoFromFileInMemory (
    load->pSrcData,
      load->SrcDataSize,
        &img_info );

  HRESULT hr = E_FAIL;

  if (img_info.Depth == 1)
  {
    hr =
      D3DXCreateTextureFromFileInMemoryEx_Original (
        load->pDevice,
            load->pSrcData, load->SrcDataSize,
              img_info.Width, img_info.Height, 0,
                0, false/*config.textures.uncompressed*/ ? D3DFMT_A8R8G8B8 : img_info.Format,
                  D3DPOOL_DEFAULT,
                    D3DX_FILTER_TRIANGLE | D3DX_FILTER_DITHER,
                    D3DX_FILTER_BOX      | D3DX_FILTER_DITHER,
                      0,
                        nullptr, nullptr,
                          &load->pSrc );
  }

  else
  {
    tex_log.Log (L"[ Tex. Mgr ] Will not resample cubemap...");
  }

  delete [] load->pSrcData;

  return hr;
}

unsigned int
__stdcall
SK::D3D9::TextureWorkerThread::ThreadProc (LPVOID user)
{
  {
    if (! streaming_memory::data_len ())
    {
      streaming_memory::data_len () = 0;
      streaming_memory::data     () = nullptr;
      streaming_memory::data_age () = 0;
    }
  }

  SYSTEM_INFO     sysinfo = { };
  GetSystemInfo (&sysinfo);


  ULONG thread_num =
    InterlockedIncrement (&num_threads_init);

  // If a system has more than 4 CPUs (logical or otherwise), let the last one
  //   be dedicated to rendering.
  ULONG processor_num = thread_num % ( sysinfo.dwNumberOfProcessors > 4 ?
                                         sysinfo.dwNumberOfProcessors - 1 :
                                         sysinfo.dwNumberOfProcessors );

  // Tales of Symphonia and Zestiria both pin the render thread to the last
  //   CPU... let's try to keep our worker threads OFF that CPU.

  SetThreadIdealProcessor (GetCurrentThread (),         processor_num);
  SetThreadAffinityMask   (GetCurrentThread (), (1UL << processor_num) & 0xFFFFFFFF);

  auto* pThread =
    static_cast <TextureWorkerThread *> (user);

  DWORD dwWaitStatus = 0;

  struct {
    const DWORD job_start  = WAIT_OBJECT_0;
    const DWORD mem_trim   = WAIT_OBJECT_0 + 1;
    const DWORD thread_end = WAIT_OBJECT_0 + 2;
  } wait;

  do
  {
    dwWaitStatus =
      WaitForMultipleObjects ( 3,
                                 pThread->control_.ops,
                                   FALSE,
                                     INFINITE );

    // New Work Ready
    if (dwWaitStatus == wait.job_start)
    {
      TexLoadRequest* pStream = pThread->job_;

      tex_mgr.injector.beginLoad ();
      {
        if (pStream->type == TexLoadRequest::Resample)
        {
          InterlockedIncrement      (&tex_mgr.injector.resampling);

          QueryPerformanceCounter   (&pStream->start);

          HRESULT hr =
            ResampleTexture (pStream);

          QueryPerformanceCounter   (&pStream->end);

          InterlockedDecrement      (&tex_mgr.injector.resampling);

          if (SUCCEEDED (hr))
            pThread->pool_->postFinished (pStream);

          else
          {
            tex_log.Log ( L"[ Tex. Mgr ] Texture Resample Failure (hr=%x) for texture %x, blacklisting from future resamples...",
                            hr, pStream->checksum );
            resample_blacklist.emplace (pStream->checksum);

            pStream->pDest->Release ();
            pStream->pSrc = pStream->pDest;

            ((ISKTextureD3D9 *)pStream->pSrc)->must_block = false;
            ((ISKTextureD3D9 *)pStream->pSrc)->refs--;

            tex_mgr.injector.finishedStreaming (pStream->checksum);
          }

          pThread->finishJob ();
        }

        else
        {
          InterlockedIncrement        (&tex_mgr.injector.streaming);
          InterlockedExchangeAdd      (&tex_mgr.injector.streaming_bytes, pStream->SrcDataSize);

          QueryPerformanceCounter     (&pStream->start);


          HRESULT hr = S_OK;

          hr =
            tex_mgr.injectTexture (pStream);

          QueryPerformanceCounter     (&pStream->end);

          InterlockedExchangeSubtract (&tex_mgr.injector.streaming_bytes, pStream->SrcDataSize);
          InterlockedDecrement        (&tex_mgr.injector.streaming);

          if (SUCCEEDED (hr))
            pThread->pool_->postFinished (pStream);

          else
          {
            tex_log.Log ( L"[ Tex. Mgr ] Texture Injection Failure (hr=%x) for texture %x, removing from injectable list...",
                            pStream->checksum);

            if (tex_mgr.isTextureInjectable     (pStream->checksum))
                tex_mgr.removeInjectableTexture (pStream->checksum);

            pStream->pSrc->Release ();
            pStream->pSrc = pStream->pDest;

            ((ISKTextureD3D9 *)pStream->pSrc)->must_block = false;
            ((ISKTextureD3D9 *)pStream->pSrc)->refs--;

            tex_mgr.injector.finishedStreaming (pStream->checksum);
          }

          pThread->finishJob ();
        }
      }
      tex_mgr.injector.endLoad ();

    }

    else if (dwWaitStatus == (wait.mem_trim))
    {
      // Yay for magic numbers :P   ==> (8 MiB Min Size, 5 Seconds Between Trims)
      //
      const size_t   MIN_SIZE = 8192 * 1024;
      const uint32_t MIN_AGE  = 5000UL;

      size_t before = streaming_memory::data_len ();

      streaming_memory::trim ( MIN_SIZE, 
                                 timeGetTime () - MIN_AGE );

      size_t now    = streaming_memory::data_len ();

      if (before != now)
      {
#ifdef _WIN64
        tex_log.Log ( L"[ Mem. Mgr ]  Trimmed %9lzu bytes of temporary memory for tid=%x",
                        before - now,
                          GetCurrentThreadId () );
#else
        tex_log.Log ( L"[ Mem. Mgr ]  Trimmed %9zu bytes of temporary memory for tid=%x",
                        before - now,
                          GetCurrentThreadId () );
#endif
      }
    }

    else if (dwWaitStatus != (wait.thread_end))
    {
      dll_log.Log ( L"[ Tex. Mgr ] Unexpected Worker Thread Wait Status: %X",
                      dwWaitStatus );
    }
  } while (dwWaitStatus != (wait.thread_end));

  streaming_memory::trim (0, timeGetTime ());

  //CloseHandle (GetCurrentThread ());
  return 0;
}

unsigned int
__stdcall
SK::D3D9::TextureThreadPool::Spooler (LPVOID user)
{
  auto* pPool =
    static_cast <TextureThreadPool *> (user);

  WaitForSingleObject (pPool->events_.jobs_added, INFINITE);

  while (WaitForSingleObject (pPool->events_.shutdown, 0) == WAIT_TIMEOUT)
  {
    TexLoadRequest* pJob =
      pPool->getNextJob ();

    while (pJob != nullptr)
    {
      bool started = false;

      for ( auto it : pPool->workers_ )
      {
        if (! it->isBusy ())
        {
          if (! started)
          {
            it->startJob (pJob);
            started = true;
          }

          else
          {
            it->trim ();
          }
        }
      }

      // All worker threads are busy, so wait...
      if (! started)
      {
        WaitForSingleObject (pPool->events_.results_waiting, INFINITE);
      }

      else
      {
        pJob =
          pPool->getNextJob ();
      }
    }

    const int MAX_TIME_BETWEEN_TRIMS = 1500UL;
    while ( WaitForSingleObject (
              pPool->events_.jobs_added,
                MAX_TIME_BETWEEN_TRIMS ) ==
                               WAIT_TIMEOUT )
    {
      for ( auto it : pPool->workers_ )
      {
        if (! it->isBusy ())
        {
          it->trim ();
        }
      }
    }
  }

  //CloseHandle (GetCurrentThread ());
  return 0;
}

void
SK::D3D9::TextureWorkerThread::finishJob (void)
{
  InterlockedExchangeAdd64   (&bytes_loaded_,
           ((TexLoadRequest *)InterlockedCompareExchangePointer ((PVOID *)&job_, nullptr, nullptr))
                             ->SrcDataSize);
  InterlockedIncrement       (&jobs_retired_);
  InterlockedExchangePointer ((PVOID *)&job_, nullptr);
}




void
SK::D3D9::TextureManager::refreshDataSources (void)
{
  if (! init)
    return;


  static bool crc_init = false;

  if (! crc_init)
  {
    CrcGenerateTable ();
    crc_init = true;
  }

  CFileInStream arc_stream  = { };
  CLookToRead   look_stream = { };

  FileInStream_CreateVTable (&arc_stream);
  LookToRead_CreateVTable   (&look_stream, false);

  look_stream.realStream = &arc_stream.s;
  LookToRead_Init         (&look_stream);

  injectable_textures.clear ();
  archives.clear            ();

  injector.lockInjection ();

  //
  // Walk injectable textures so we don't have to query the filesystem on every
  //   texture load to check if a injectable one exists.
  //
  if ( GetFileAttributesW ((SK_D3D11_res_root + L"\\inject").c_str ()) !=
         INVALID_FILE_ATTRIBUTES )
  {
    WIN32_FIND_DATA fd;
    HANDLE          hFind  = INVALID_HANDLE_VALUE;
    int             files  = 0;
    LARGE_INTEGER   liSize = { 0 };

    tex_log.LogEx ( true, L"[Inject Tex] Enumerating injectable textures..." );

    hFind =
      FindFirstFileW ((SK_D3D11_res_root + L"\\inject\\textures\\blocking\\*").c_str (), &fd);

    if (hFind != INVALID_HANDLE_VALUE)
    {
      do
      {
        if (fd.dwFileAttributes != INVALID_FILE_ATTRIBUTES)
        {
          if (wcsstr (_wcslwr (fd.cFileName), L".dds")) 
{
            uint32_t checksum;
            swscanf (fd.cFileName, L"%x.dds", &checksum);

            // Already got this texture...
            if (injectable_textures.count (checksum))
                continue;

            ++files;

            LARGE_INTEGER fsize;

            fsize.HighPart = fd.nFileSizeHigh;
            fsize.LowPart  = fd.nFileSizeLow;

            liSize.QuadPart += fsize.QuadPart;

            TexRecord rec = { };

            rec.size    = liSize.LowPart;
            rec.archive = std::numeric_limits <unsigned int>::max ();
            rec.method  = Blocking;

            injectable_textures.emplace (std::make_pair (checksum, rec));
          }
        }
      } while (FindNextFileW (hFind, &fd) != 0);

      FindClose (hFind);
    }

    hFind =
      FindFirstFileW ((SK_D3D11_res_root + L"\\inject\\textures\\streaming\\*").c_str (), &fd);

    if (hFind != INVALID_HANDLE_VALUE)
    {
      do
      {
        if (fd.dwFileAttributes != INVALID_FILE_ATTRIBUTES)
        {
          if (wcsstr (_wcslwr (fd.cFileName), L".dds"))
          {
            uint32_t checksum;
            swscanf (fd.cFileName, L"%x.dds", &checksum);

            // Already got this texture...
            if (injectable_textures.count (checksum))
                continue;

            ++files;

            LARGE_INTEGER fsize;

            fsize.HighPart = fd.nFileSizeHigh;
            fsize.LowPart  = fd.nFileSizeLow;

            liSize.QuadPart += fsize.QuadPart;

            TexRecord rec = { };

            rec.size    = fsize.LowPart;
            rec.archive = std::numeric_limits <unsigned int>::max ();
            rec.method  = Streaming;

            injectable_textures.emplace (std::make_pair (checksum, rec));
          }
        }
      } while (FindNextFileW (hFind, &fd) != 0);

      FindClose (hFind);
    }

    hFind =
      FindFirstFileW ((SK_D3D11_res_root + L"\\inject\\textures\\*").c_str (), &fd);

    if (hFind != INVALID_HANDLE_VALUE)
    {
      do
      {
        if (fd.dwFileAttributes != INVALID_FILE_ATTRIBUTES)
        {
          if (wcsstr (_wcslwr (fd.cFileName), L".dds"))
          {
            uint32_t checksum;
            swscanf (fd.cFileName, L"%x.dds", &checksum);

            // Already got this texture...
            if (injectable_textures.count (checksum))
                continue;

            ++files;

            LARGE_INTEGER fsize;

            fsize.HighPart = fd.nFileSizeHigh;
            fsize.LowPart  = fd.nFileSizeLow;

            liSize.QuadPart += fsize.QuadPart;

            TexRecord rec = { };

            rec.size    = fsize.LowPart;
            rec.archive = std::numeric_limits <unsigned int>::max ();
            rec.method  = DontCare;

            if (! injectable_textures.count (checksum))
              injectable_textures.emplace (std::make_pair (checksum, rec));
          }
        }
      } while (FindNextFileW (hFind, &fd) != 0);

      FindClose (hFind);
    }

    hFind =
      FindFirstFileW ((SK_D3D11_res_root + L"\\inject\\*.*").c_str (), &fd);

    if (hFind != INVALID_HANDLE_VALUE)
    {
      int archive = 0;

      do
      {
        if (fd.dwFileAttributes != INVALID_FILE_ATTRIBUTES)
        {
          wchar_t* wszArchiveNameLwr =
            _wcslwr (_wcsdup (fd.cFileName));

          if ( wcsstr (wszArchiveNameLwr, L".7z") )
          {
            int tex_count = 0;

            CSzArEx       arc = { };
            ISzAlloc      thread_alloc;
            ISzAlloc      thread_tmp_alloc;

            thread_alloc.Alloc     = SzAlloc;
            thread_alloc.Free      = SzFree;

            thread_tmp_alloc.Alloc = SzAllocTemp;
            thread_tmp_alloc.Free  = SzFreeTemp;

            wchar_t wszQualifiedArchiveName [MAX_PATH] = { };

            _swprintf ( wszQualifiedArchiveName,
                          L"%s\\inject\\%s",
                            SK_D3D11_res_root.c_str (),
                              fd.cFileName );

            if (InFile_OpenW (&arc_stream.file, wszQualifiedArchiveName))
            {
              tex_log.Log ( L"[Inject Tex]  ** Cannot open archive file: %s",
                              wszQualifiedArchiveName );
              continue;
            }

            SzArEx_Init (&arc);

            if ( SzArEx_Open ( &arc,
                                 &look_stream.s,
                                   &thread_alloc,
                                     &thread_tmp_alloc ) == SZ_OK )
            {
              uint32_t i;

              wchar_t wszEntry [MAX_PATH];

              for (i = 0; i < arc.NumFiles; i++)
              {
                if (SzArEx_IsDir (&arc, i))
                  continue;

                SzArEx_GetFileNameUtf16 (&arc, i, (UInt16 *)wszEntry);

                // Truncate to 32-bits --> there's no way in hell a texture will ever be >= 2 GiB
                UInt64 fileSize = SzArEx_GetFileSize (&arc, i);

                wchar_t* wszFullName =
                  _wcslwr (_wcsdup (wszEntry));

                if ( wcsstr ( wszFullName, L".dds") )
                {
                  TexLoadMethod method = DontCare;

                  uint32_t checksum = 0x00;
                  wchar_t* wszUnqualifiedEntry =
                    wszFullName + wcslen (wszFullName);

                  // Strip the path
                  while (  wszUnqualifiedEntry >= wszFullName &&
                          *wszUnqualifiedEntry != L'/')
                    wszUnqualifiedEntry = CharPrevW (wszFullName, wszUnqualifiedEntry);

                  if (*wszUnqualifiedEntry == L'/')
                    wszUnqualifiedEntry = CharNextW (wszUnqualifiedEntry);

                  swscanf (wszUnqualifiedEntry, L"%x.dds", &checksum);

                  // Already got this texture...
                  if ( isTextureInjectable  (checksum) ||
                       isTextureBlacklisted (checksum) )
                  {
                    free (wszFullName);
                    continue;
                  }

                  if (wcsstr (wszFullName, L"streaming"))
                    method = Streaming;
                  else if (wcsstr (wszFullName, L"blocking"))
                    method = Blocking;

                  TexRecord rec = { };

                  rec.size    = (uint32_t)fileSize;
                  rec.archive = archive;
                  rec.fileno  = i;
                  rec.method  = method;

                  injectable_textures.emplace (std::make_pair (checksum, rec));

                  ++tex_count;
                  ++files;

                  liSize.QuadPart += rec.size;
                }

                free (wszFullName);
              }

              if (tex_count > 0)
              {
                ++archive;
                archives.emplace_back (wszQualifiedArchiveName);
              }
            }

            SzArEx_Free (&arc, &thread_alloc);
            File_Close  (&arc_stream.file);
          }

          free (wszArchiveNameLwr);
        }
      } while (FindNextFileW (hFind, &fd) != 0);

      FindClose (hFind);
    }

    tex_log.LogEx ( false, L" %lu files (%3.1f MiB)\n",
                      files, (double)liSize.QuadPart / (1024.0 * 1024.0) );
  }

  injector.unlockInjection ();

  File_Close  (&arc_stream.file);
}


bool
SK::D3D9::TextureManager::TextureManager::reloadTexture (uint32_t checksum)
{
  if (! init)
    return false;

  if ( ! isTextureInjectable (checksum) )
    return false;

  injector.lockStreaming ();

  Texture* pCacheTex =
    getTexture (checksum);

  ISKTextureD3D9* pTex = 
    pCacheTex ? pCacheTex->d3d9_tex :
                nullptr;

  if (pTex != nullptr && pTex->pTexOverride != nullptr)
  {
    tex_log.LogEx ( true, L"[Inject Tex] Reloading texture for checksum (%08x)... ",
                      checksum );

    InterlockedDecrement (&injected_count);
    InterlockedAdd64     (&injected_size, -pTex->override_size);

    pTex->pTexOverride->Release ();
    pTex->pTexOverride = nullptr;
  }

  else
  {
    injector.unlockStreaming ();

    return false;
  }

  TexRecord record =
    getInjectableTexture (checksum);

  if (record.method == DontCare)
    record.method = Streaming;

  TexLoadRequest* load_op = nullptr;

  wchar_t wszInjectFileName [MAX_PATH] = { L'\0' };

  bool remap_stream =
    injector.isStreaming (checksum);

  // If -1, load from disk...
  if (record.archive == std::numeric_limits <unsigned int>::max ())
  {
    if (record.method == Streaming)
    {
      _swprintf ( wszInjectFileName, L"%s\\inject\\textures\\streaming\\%08x%s",
                    SK_D3D11_res_root.c_str (),
                      checksum,
                        L".dds" );
    }

    else if (record.method == Blocking)
    {
      _swprintf ( wszInjectFileName, L"%s\\inject\\textures\\blocking\\%08x%s",
                    SK_D3D11_res_root.c_str (),
                      checksum,
                        L".dds" );
    }
  }

  load_op           = new TexLoadRequest ();

  SK_GetCurrentRenderBackend ().device->QueryInterface <IDirect3DDevice9> (&load_op->pDevice);

  load_op->checksum = checksum;
  load_op->type     = TexLoadRequest::Stream;

  wcscpy (load_op->wszFilename, wszInjectFileName);

  if (load_op->type == TexLoadRequest::Stream)
  {
    if ((! remap_stream))
      tex_log.LogEx ( false, L"streaming\n" );
    else
      tex_log.LogEx ( false, L"in-flight already\n" );
  }

  load_op->SrcDataSize =
    isTextureInjectable (checksum) ?
      (UINT)injectable_textures [checksum].size : 0;

  load_op->pDest = pTex;

  pTex->must_block = false;

  if (injector.isStreaming (load_op->checksum))
  {
    //ISKTextureD3D9* pTexOrig =
    //  (ISKTextureD3D9 *)injector.getTextureInFlight (load_op->checksum)->pDest;

    // Remap the output of the in-flight texture
    injector.getTextureInFlight (load_op->checksum)->pDest =
      pTex;

    Texture* pTexCache = getTexture (load_op->checksum);

    if (pTexCache != nullptr)
    {
      for ( int i = 0;
                i < pTexCache->refs;
              ++i )
      {
        pTex->AddRef ();
      }
    }
  }

  else
  {
    injector.addTextureInFlight (load_op);
    load_op->pDest->AddRef      (       );
    stream_pool.postJob         (load_op);
  }

  injector.unlockStreaming ();

  if (injector.hasPendingLoads ())
    loadQueuedTextures ();

  return true;
}



void
SK::D3D9::TextureManager::getThreadStats (std::vector <TexThreadStats>& stats)
{
  stats =
    resample_pool->getWorkerStats ();

  // For Inject (Small, Large) -> Push Back
}


void
SK::D3D9::TextureThreadPool::postJob (TexLoadRequest* job)
{
  EnterCriticalSection (&cs_jobs);
  {
    // Defer the creation of this until the first job is posted
    if (! spool_thread_)
    {
      spool_thread_ =
        (HANDLE)_beginthreadex ( nullptr,
                                   0,
                                     Spooler,
                                       this,
                                         0x00,
                                           nullptr );
    }

    // Don't let the game free this while we are working on it...
    job->pDest->AddRef ();
  
    jobs_.push           (job);

    LeaveCriticalSection (&cs_jobs);
    SetEvent             (events_.jobs_added);
  }
}



IDirect3DTexture9*
ISKTextureD3D9::getDrawTexture (void) const
{
  IDirect3DTexture9* pTexToUse = nullptr;

  switch (img_to_use)
  {
    case ContentPreference::DontCare:
    {
      if (__remap_textures && pTexOverride != nullptr)
        pTexToUse = pTexOverride;

      else
        pTexToUse = pTex;
    } break;

    case ContentPreference::Original:
    {
      pTexToUse = pTex;
    } break;

    case ContentPreference::Override:
    {
      if (pTexOverride != nullptr)
        pTexToUse = pTexOverride;
      else
        pTexToUse = pTex;
    } break;
  };

  if (debug_tex_id > 0 && tex_crc32c == (uint32_t)debug_tex_id && config.textures.highlight_debug_tex)
  {
    extern DWORD tracked_tex_blink_duration;

    if (timeGetTime () % tracked_tex_blink_duration > tracked_tex_blink_duration / 2)
      pTexToUse = nullptr;
  }

  return pTexToUse;
}

IDirect3DTexture9*
ISKTextureD3D9::use (void)
{
  if (config.textures.dump_on_load && pTex != nullptr && uses > 0 && uses < 2)
  {
    D3DSURFACE_DESC desc = { };

    if ( SUCCEEDED (pTex->GetLevelDesc (0, &desc)) &&
      ( ! ( SK::D3D9::tex_mgr.isTextureDumped     (tex_crc32c) ||
            SK::D3D9::tex_mgr.isTextureInjectable (tex_crc32c) ) ) &&
          (desc.Pool != D3DPOOL_MANAGED) )
    {
      tex_log.Log ( L"[Dump Trace] Texture:   (%lu x %lu) * <LODs: %lu> - CRC32C: %08X",
                       desc.Width, desc.Height, pTex->GetLevelCount (), tex_crc32c);
      tex_log.Log ( L"[Dump Trace]              Usage: %-20s - Format: %-20s",
                       SK_D3D9_UsageToStr  (desc.Usage).c_str  (),
                       SK_D3D9_FormatToStr (desc.Format).c_str ());
      tex_log.Log ( L"[Dump Trace]               Pool: %s",
                       SK_D3D9_PoolToStr (desc.Pool));

      SK::D3D9::tex_mgr.injector.beginLoad ();

      SK::D3D9::tex_mgr.dumpTexture (desc.Format, tex_crc32c, pTex);

      SK::D3D9::tex_mgr.injector.endLoad ();
    }
  }

  ++uses;

  QueryPerformanceCounter_Original (&last_used);

  return getDrawTexture ();
}

void
ISKTextureD3D9::toggleOverride (void)
{
  if (img_to_use != ContentPreference::Override)
    img_to_use = ContentPreference::Override;
  else
    img_to_use = ContentPreference::DontCare;
}

void
ISKTextureD3D9::toggleOriginal (void)
{
  if (img_to_use != ContentPreference::Original)
    img_to_use = ContentPreference::Original;
  else
    img_to_use = ContentPreference::DontCare;
}















std::unordered_map   <uint32_t, TexLoadRequest *> SK::D3D9::TextureManager::Injector::textures_in_flight;
std::queue <TexLoadRef>                           SK::D3D9::TextureManager::Injector::textures_to_stream;
std::queue <TexLoadRef>                           SK::D3D9::TextureManager::Injector::finished_loads;

CRITICAL_SECTION                                  SK::D3D9::TextureManager::Injector::cs_tex_stream    = { };
CRITICAL_SECTION                                  SK::D3D9::TextureManager::Injector::cs_tex_resample  = { };
CRITICAL_SECTION                                  SK::D3D9::TextureManager::Injector::cs_tex_inject    = { };
CRITICAL_SECTION                                  SK::D3D9::TextureManager::Injector::cs_tex_dump      = { };
CRITICAL_SECTION                                  SK::D3D9::TextureManager::Injector::cs_tex_blacklist = { };

volatile  LONG                                    SK::D3D9::TextureManager::Injector::streaming          = 0L;
volatile ULONG                                    SK::D3D9::TextureManager::Injector::streaming_bytes    = 0UL;

volatile  LONG                                    SK::D3D9::TextureManager::Injector::resampling         = 0L;