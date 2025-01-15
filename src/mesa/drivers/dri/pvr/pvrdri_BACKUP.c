/* -*- mode: c; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/* vi: set ts=8 sw=8 sts=8: */
/*
 * Copyright (c) Imagination Technologies Ltd.
 *
 * The contents of this file are subject to the MIT license as set out below.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <assert.h>
#include <pthread.h>
#include <string.h>
//#include <xf86drm.h>
#include "drm.h"

#include "EGL/egl.h"
#include "EGL/eglext.h"

#include "dri_util.h"

#include "pvrdri.h"
#include "pvrimage.h"

#include "pvrmesa.h"

#define PVR_IMAGE_LOADER_VER_MIN 1
#define PVR_DRI2_LOADER_VER_MIN 3

#define PVRDRI_FLUSH_WAIT_FOR_HW        (1U << 0)
#define PVRDRI_FLUSH_NEW_EXTERNAL_FRAME (1U << 1)
#define PVRDRI_FLUSH_ALL_SURFACES       (1U << 2)

typedef struct PVRBufferRec
{
	__DRIbuffer sDRIBuffer;
	PVRDRIBufferImpl *psImpl;
} PVRBuffer;

extern const __DRIextension *apsScreenExtensions[];
extern const __DRIextension asScreenExtensionVersionInfo[];

/* We need to know the current screen in order to lookup EGL images. */
static __thread PVRDRIScreen *gpsPVRScreen;

/*************************************************************************/ /*!
 Local functions
*/ /**************************************************************************/

static bool PVRLoaderIsSupported(__DRIscreen *psDRIScreen)
{
	if (psDRIScreen->image.loader)
	{
		if (psDRIScreen->image.loader->base.version < PVR_IMAGE_LOADER_VER_MIN)
		{
			__driUtilMessage("%s: Image loader extension version %d but need %d",
					 __func__,
					 psDRIScreen->image.loader->base.version,
					 PVR_IMAGE_LOADER_VER_MIN);
			return false;
		}
		else if (!psDRIScreen->image.loader->getBuffers)
		{
			__driUtilMessage("%s: Image loader extension missing support for getBuffers",
					 __func__);
			return false;
		}
	}
	else if (psDRIScreen->dri2.loader)
	{
		if (psDRIScreen->dri2.loader->base.version < PVR_DRI2_LOADER_VER_MIN)
		{
			__driUtilMessage("%s: DRI2 loader extension version %d but need %d",
					 __func__,
					 psDRIScreen->dri2.loader->base.version,
					 PVR_DRI2_LOADER_VER_MIN);
			return false;
		}
		else if (!psDRIScreen->dri2.loader->getBuffersWithFormat)
		{
			__driUtilMessage("%s: DRI2 loader extension missing support for getBuffersWithFormat",
					 __func__);
			return false;
		}
	}
	else
	{
		__driUtilMessage("%s: Missing required loader extension (need "
				 "either the image or DRI2 loader extension)",
				 __func__);
		return false;
	}

	return true;
}

static bool PVRMutexInit(pthread_mutex_t *psMutex, int iType)
{
	pthread_mutexattr_t sMutexAttr;
	int res;

	res = pthread_mutexattr_init(&sMutexAttr);
	if (res != 0)
	{
		__driUtilMessage("%s: pthread_mutexattr_init failed (%d)",
				 __func__,
				 res);
		return false;
	}

	res = pthread_mutexattr_settype(&sMutexAttr, iType);
	if (res != 0)
	{
		__driUtilMessage("%s: pthread_mutexattr_settype failed (%d)",
				 __func__,
				 res);
		goto ErrorMutexAttrDestroy;
	}

	res = pthread_mutex_init(psMutex, &sMutexAttr);
	if (res != 0)
	{
		__driUtilMessage("%s: pthread_mutex_init failed (%d)",
				 __func__,
				 res);
		goto ErrorMutexAttrDestroy;
	}

	(void) pthread_mutexattr_destroy(&sMutexAttr);

	return true;

ErrorMutexAttrDestroy:
	(void) pthread_mutexattr_destroy(&sMutexAttr);

	return false;
}

static void PVRMutexDeinit(pthread_mutex_t *psMutex)
{
	int res;

	res = pthread_mutex_destroy(psMutex);
	if (res != 0)
	{
		__driUtilMessage("%s: pthread_mutex_destroy failed (%d)",
				 __func__,
				 res);
	}
}

static inline bool
PVRDRIFlushBuffers(PVRDRIContext *psPVRContext,
		   PVRDRIDrawable *psPVRDrawable,
		   uint32_t uiFlags)
{
	assert(!(uiFlags & ~(PVRDRI_FLUSH_WAIT_FOR_HW |
			     PVRDRI_FLUSH_NEW_EXTERNAL_FRAME |
			     PVRDRI_FLUSH_ALL_SURFACES)));

	return PVRDRIEGLFlushBuffers(psPVRContext->eAPI,
	                             psPVRContext->psPVRScreen->psImpl,
	                             psPVRContext->psImpl,
	                             psPVRDrawable ? psPVRDrawable->psImpl : NULL,
	                             uiFlags & PVRDRI_FLUSH_ALL_SURFACES,
	                             uiFlags & PVRDRI_FLUSH_NEW_EXTERNAL_FRAME,
	                             uiFlags & PVRDRI_FLUSH_WAIT_FOR_HW);
}

bool
PVRDRIFlushBuffersForSwap(PVRDRIContext *psPVRContext,
                          PVRDRIDrawable *psPVRDrawable)
{
	PVRDRIContext *psPVRFlushContext;
	uint32_t uiFlushFlags;
	uint32_t uiFlags = 0;
	q_elem_t *psQElem;

	if (psPVRContext)
	{
		return PVRDRIFlushBuffers(psPVRContext, psPVRDrawable,
					  PVRDRI_FLUSH_NEW_EXTERNAL_FRAME);
	}

	for (psQElem = psPVRDrawable->sPVRContextHead.forward;
	     psQElem != &psPVRDrawable->sPVRContextHead;
	     psQElem = psPVRFlushContext->sQElem.forward)
	{
		psPVRFlushContext =
			QUEUE_CONTAINER_OF(psQElem, PVRDRIContext, sQElem);
		uiFlushFlags = uiFlags;

		if (!psPVRContext || psPVRContext == psPVRFlushContext)
		{
			uiFlushFlags |= PVRDRI_FLUSH_NEW_EXTERNAL_FRAME;
		}

		(void) PVRDRIFlushBuffers(psPVRFlushContext, psPVRDrawable,
					  uiFlushFlags);
	}

	return true;
}

static bool
PVRDRIFlushBuffersGC(PVRDRIContext *psPVRContext)
{
		return PVRDRIFlushBuffers(psPVRContext, NULL,
					  PVRDRI_FLUSH_WAIT_FOR_HW |
					  PVRDRI_FLUSH_ALL_SURFACES);
}

static void PVRDRIDisplayFrontBuffer(PVRDRIDrawable *psPVRDrawable)
{
	if (!psPVRDrawable->bDoubleBuffered)
	{
		PVRDRIScreen *psPVRScreen = psPVRDrawable->psPVRScreen;
		__DRIscreen *psDRIScreen = psPVRScreen->psDRIScreen;

		if (psDRIScreen->image.loader && psDRIScreen->image.loader->flushFrontBuffer)
		{
			psDRIScreen->image.loader->flushFrontBuffer(psPVRDrawable->psDRIDrawable,
								    psPVRDrawable->psDRIDrawable->loaderPrivate);
		}
		else if (psDRIScreen->dri2.loader && psDRIScreen->dri2.loader->flushFrontBuffer)
		{
			psDRIScreen->dri2.loader->flushFrontBuffer(psPVRDrawable->psDRIDrawable,
								   psPVRDrawable->psDRIDrawable->loaderPrivate);
		}
	}
}

static void PVRContextUnbind(PVRDRIContext *psPVRContext,
			     bool bMakeUnCurrent,
			     bool bMarkSurfaceInvalid)
{
	if (bMakeUnCurrent || psPVRContext->psPVRDrawable != NULL)
	{
		(void) PVRDRIFlushBuffersGC(psPVRContext);
	}

	if (bMakeUnCurrent)
	{
		PVRDRIMakeUnCurrentGC(psPVRContext->eAPI,
		                      psPVRContext->psPVRScreen->psImpl);
	}

	if (psPVRContext->psPVRDrawable != NULL)
	{
		if (bMarkSurfaceInvalid)
		{
			PVRDRIEGLMarkRendersurfaceInvalid(psPVRContext->eAPI,
			                                  psPVRContext->psPVRScreen->psImpl,
			                                  psPVRContext->psImpl);
		}

		psPVRContext->psPVRDrawable = NULL;
	}

	queue_dequeue(&psPVRContext->sQElem);
}

static inline PVRDRIContextImpl *
getSharedContextImpl(void *pvSharedContextPrivate)
{
	if (pvSharedContextPrivate == NULL)
	{
		return NULL;
	}
	return ((PVRDRIContext *)pvSharedContextPrivate)->psImpl;
}


static inline void PVRDRIConfigFromMesa(PVRDRIConfigInfo *psConfigInfo,
                                        const struct gl_config *psGLMode)
{
	memset(psConfigInfo, 0, sizeof(*psConfigInfo));

	if (psGLMode)
	{
		psConfigInfo->samples           = psGLMode->samples;
		psConfigInfo->redBits           = psGLMode->redBits;
		psConfigInfo->greenBits         = psGLMode->greenBits;
		psConfigInfo->blueBits          = psGLMode->blueBits;
		psConfigInfo->alphaBits         = psGLMode->alphaBits;
		psConfigInfo->rgbBits           = psGLMode->rgbBits;
		psConfigInfo->depthBits         = psGLMode->depthBits;
		psConfigInfo->stencilBits       = psGLMode->stencilBits;
		psConfigInfo->doubleBufferMode  = psGLMode->doubleBufferMode;

		psConfigInfo->sampleBuffers     = psGLMode->sampleBuffers;
		psConfigInfo->bindToTextureRgb  = psGLMode->bindToTextureRgb;
		psConfigInfo->bindToTextureRgba = psGLMode->bindToTextureRgba;
	}
}

static void PVRDRIScreenAddReference(PVRDRIScreen *psPVRScreen)
{
	int iRefCount = __sync_fetch_and_add(&psPVRScreen->iRefCount, 1);
	(void)iRefCount;
	assert(iRefCount > 0);
}

static void PVRDRIScreenRemoveReference(PVRDRIScreen *psPVRScreen)
{
	int iRefCount = __sync_sub_and_fetch(&psPVRScreen->iRefCount, 1);

	assert(iRefCount >= 0);

	if (iRefCount)
	{
		return;
	}

	pvrdri_free_dispatch_tables(psPVRScreen);

	(void)PVRDRIEGLFreeResources(psPVRScreen->psImpl);

	PVRDRIDestroyFencesImpl(psPVRScreen->psImpl);

	PVRDRIDestroyScreenImpl(psPVRScreen->psImpl);
	PVRMutexDeinit(&psPVRScreen->sMutex);

	free(psPVRScreen);
}

static inline void PVRDrawableUnbindContexts(PVRDRIDrawable *psPVRDrawable)
{
	q_elem_t *psQElem = psPVRDrawable->sPVRContextHead.forward;

	while (psQElem != &psPVRDrawable->sPVRContextHead)
	{
		PVRDRIContext *psPVRContext = QUEUE_CONTAINER_OF(psQElem,
		                                                 PVRDRIContext,
		                                                 sQElem);

		/* Get the next element in the list now, as the list will be modified */
		psQElem = psPVRContext->sQElem.forward;

		/* Draw surface? */
		if (psPVRContext->psPVRDrawable == psPVRDrawable)
		{
			PVRContextUnbind(psPVRContext, false, true);
		}
		/* Pixmap? */
		else
		{
			(void) PVRDRIFlushBuffersGC(psPVRContext);
			queue_dequeue(&psPVRContext->sQElem);
		}
	}
}

static void PVRScreenPrintExtensions(__DRIscreen *psDRIScreen)
{
	/* Don't attempt to print anything if LIBGL_DEBUG isn't in the environment */
	if (getenv("LIBGL_DEBUG") == NULL)
	{
		return;
	}

	if (psDRIScreen->extensions)
	{
		int i;
		int j;

		__driUtilMessage("Supported screen extensions:");

		for (i = 0; psDRIScreen->extensions[i]; i++)
		{
			for (j = 0; asScreenExtensionVersionInfo[j].name; j++)
			{
				if (strcmp(psDRIScreen->extensions[i]->name,
				           asScreenExtensionVersionInfo[j].name) == 0)
				{
					__driUtilMessage("\t%s (supported version: %u - max version: %u)",
							 psDRIScreen->extensions[i]->name,
							 psDRIScreen->extensions[i]->version,
							 asScreenExtensionVersionInfo[j].version);
					break;
				}
			}

			if (asScreenExtensionVersionInfo[j].name == NULL)
			{
				__driUtilMessage("\t%s (supported version: %u - max version: unknown)",
						 psDRIScreen->extensions[i]->name,
						 psDRIScreen->extensions[i]->version);
			}
		}
	}
	else
	{
		__driUtilMessage("No screen extensions found");
	}
}


/*************************************************************************/ /*!
 Mesa driver API functions
*/ /**************************************************************************/
static const __DRIconfig **PVRDRIInitScreen(__DRIscreen *psDRIScreen)
{
	PVRDRIScreen *psPVRScreen;
	const __DRIconfig **configs;
	PVRDRICallbacks sDRICallbacks = {
		.DrawableRecreate            = PVRDRIDrawableRecreate,
		.DrawableGetParameters       = PVRDRIDrawableGetParameters,
		.ImageGetSharedType          = PVRDRIImageGetSharedType,
		.ImageGetSharedBuffer        = PVRDRIImageGetSharedBuffer,
		.ImageGetSharedEGLImage      = PVRDRIImageGetSharedEGLImage,
		.ImageGetEGLImage            = PVRDRIImageGetEGLImage,
		.ScreenGetDRIImage           = PVRDRIScreenGetDRIImage,
		.RefImage                    = PVRDRIRefImage,
		.UnrefImage                  = PVRDRIUnrefImage,
	};

	if (!PVRLoaderIsSupported(psDRIScreen))
	{
		return NULL;
	}

	PVRDRIRegisterCallbacks(&sDRICallbacks);

	psPVRScreen = calloc(1, sizeof(*psPVRScreen));
	if (psPVRScreen == NULL)
	{
		__driUtilMessage("%s: Couldn't allocate PVRDRIScreen",
				 __func__);
		return NULL;
	}

	DRIScreenPrivate(psDRIScreen) = psPVRScreen;
	psPVRScreen->psDRIScreen = psDRIScreen;

	/*
	 * KEGLGetDrawableParameters could be called with the mutex either
	 * locked or unlocked, hence the use of a recursive mutex.
	 */
	if (!PVRMutexInit(&psPVRScreen->sMutex, PTHREAD_MUTEX_RECURSIVE))
	{
		__driUtilMessage("%s: Screen mutex initialisation failed",
				 __func__);
		goto ErrorScreenFree;
	}

	psPVRScreen->iRefCount = 1;
	psPVRScreen->bUseInvalidate = (psDRIScreen->dri2.useInvalidate != NULL);

	psDRIScreen->extensions = apsScreenExtensions;

	psPVRScreen->psImpl = PVRDRICreateScreenImpl(psDRIScreen->fd);
	if (psPVRScreen->psImpl == NULL)
	{
		goto ErrorScreenMutexDeinit;
	}

	/*
	 * OpenGL doesn't support concurrent EGL displays so only advertise
	 * OpenGL support for the first display.
	 */
	if (PVRDRIIsFirstScreen(psPVRScreen->psImpl))
	{
		psDRIScreen->max_gl_compat_version =
				PVRDRIAPIVersion(PVRDRI_API_GL,
						 PVRDRI_API_SUB_GL_COMPAT,
						 psPVRScreen->psImpl);
		psDRIScreen->max_gl_core_version =
				PVRDRIAPIVersion(PVRDRI_API_GL,
					         PVRDRI_API_SUB_GL_CORE,
					         psPVRScreen->psImpl);
	}

	psDRIScreen->max_gl_es1_version =
				PVRDRIAPIVersion(PVRDRI_API_GLES1,
					         PVRDRI_API_SUB_NONE,
					         psPVRScreen->psImpl);

	psDRIScreen->max_gl_es2_version =
				PVRDRIAPIVersion(PVRDRI_API_GLES2,
					         PVRDRI_API_SUB_NONE,
					         psPVRScreen->psImpl);

	configs = PVRDRICreateConfigs();
	if (configs == NULL)
	{
		__driUtilMessage("%s: No framebuffer configs", __func__);
		goto ErrorScreenImplDeinit;
	}

	PVRScreenPrintExtensions(psDRIScreen);

	return configs;

ErrorScreenImplDeinit:
	PVRDRIDestroyScreenImpl(psPVRScreen->psImpl);

ErrorScreenMutexDeinit:
	PVRMutexDeinit(&psPVRScreen->sMutex);

ErrorScreenFree:
	free(psPVRScreen);

	return NULL;
}

static void PVRDRIDestroyScreen(__DRIscreen *psDRIScreen)
{
	PVRDRIScreenRemoveReference(DRIScreenPrivate(psDRIScreen));
}

static EGLint PVRDRIScreenSupportedAPIs(PVRDRIScreen *psPVRScreen)
{
	unsigned api_mask = psPVRScreen->psDRIScreen->api_mask;
	EGLint supported = 0;

	if ((api_mask & (1 << __DRI_API_OPENGL)) != 0)
	{
		supported |= PVRDRI_API_BIT_GL;
	}

	if ((api_mask & (1 << __DRI_API_GLES)) != 0)
	{
		supported |= PVRDRI_API_BIT_GLES;
	}

	if ((api_mask & (1 << __DRI_API_GLES2)) != 0)
	{
		supported |= PVRDRI_API_BIT_GLES2;
	}

	if ((api_mask & (1 << __DRI_API_OPENGL_CORE)) != 0)
	{
		supported |= PVRDRI_API_BIT_GL;
	}

	if ((api_mask & (1 << __DRI_API_GLES3)) != 0)
	{
		supported |= PVRDRI_API_BIT_GLES3;
	}

	return supported;
}

static GLboolean PVRDRICreateContext(gl_api eMesaAPI,
                                     const struct gl_config *psGLMode,
                                     __DRIcontext *psDRIContext,
                                     unsigned uMajorVersion,
                                     unsigned uMinorVersion,
                                     uint32_t uFlags,
                                     bool notify_reset,
                                     unsigned *puError,
                                     void *pvSharedContextPrivate)
{
	__DRIscreen *psDRIScreen = psDRIContext->driScreenPriv;
	PVRDRIScreen *psPVRScreen = (PVRDRIScreen *)DRIScreenPrivate(psDRIScreen);
	PVRDRIContext *psPVRContext;
	unsigned uPriority;
	PVRDRIAPISubType eAPISub = PVRDRI_API_SUB_NONE;
	PVRDRIConfigInfo sConfigInfo;
	bool bResult;

	psPVRContext = calloc(1, sizeof(*psPVRContext));
	if (psPVRContext == NULL)
	{
		__driUtilMessage("%s: Couldn't allocate PVRDRIContext",
				 __func__);
		*puError = __DRI_CTX_ERROR_NO_MEMORY;
		return GL_FALSE;
	}

	psPVRContext->psDRIContext = psDRIContext;
	psPVRContext->psPVRScreen = psPVRScreen;

#if defined(__DRI_PRIORITY)
	uPriority = psDRIContext->priority;
#else
	uPriority = PVRDRI_CONTEXT_PRIORITY_MEDIUM;
#endif

	switch (eMesaAPI)
	{
		case API_OPENGL_COMPAT:
			psPVRContext->eAPI = PVRDRI_API_GL;
			eAPISub =  PVRDRI_API_SUB_GL_COMPAT;
			break;
		case API_OPENGL_CORE:
			psPVRContext->eAPI = PVRDRI_API_GL;
			eAPISub =  PVRDRI_API_SUB_GL_CORE;
			break;
		case API_OPENGLES:
			psPVRContext->eAPI = PVRDRI_API_GLES1;
			break;
		case API_OPENGLES2:
			psPVRContext->eAPI = PVRDRI_API_GLES2;
			break;
		default:
			__driUtilMessage("%s: Unsupported API: %d",
					 __func__,
					 (int)eMesaAPI);
			goto ErrorContextFree;
	}

	PVRDRIConfigFromMesa(&sConfigInfo, psGLMode);

	*puError = PVRDRICreateContextImpl(&psPVRContext->psImpl,
					   psPVRContext->eAPI,
					   eAPISub,
					   psPVRScreen->psImpl,
					   &sConfigInfo,
					   uMajorVersion,
					   uMinorVersion,
					   uFlags,
					   notify_reset,
					   uPriority,
					   getSharedContextImpl(pvSharedContextPrivate));
	if (*puError != __DRI_CTX_ERROR_SUCCESS)
	{
		goto ErrorContextFree;
	}

	/*
	 * The dispatch table must be created after the context, because
	 * PVRDRIContextCreate loads the API library, and we need the
	 * library handle to populate the dispatch table.
	 */
	PVRDRIScreenLock(psPVRScreen);
	bResult = pvrdri_create_dispatch_table(psPVRScreen, psPVRContext->eAPI);
	PVRDRIScreenUnlock(psPVRScreen);

	if (!bResult)
	{
		__driUtilMessage("%s: Couldn't create dispatch table",
				 __func__);
		*puError = __DRI_CTX_ERROR_BAD_API;
		goto ErrorContextDestroy;
	}

	psDRIContext->driverPrivate = (void *)psPVRContext;
	PVRDRIScreenAddReference(psPVRScreen);

	*puError = __DRI_CTX_ERROR_SUCCESS;

	return GL_TRUE;

ErrorContextDestroy:
	PVRDRIDestroyContextImpl(psPVRContext->psImpl,
				 psPVRContext->eAPI,
				 psPVRScreen->psImpl);
ErrorContextFree:
	free(psPVRContext);

	return GL_FALSE;
}

static void PVRDRIDestroyContext(__DRIcontext *psDRIContext)
{
	PVRDRIContext *psPVRContext = (PVRDRIContext *)psDRIContext->driverPrivate;
	PVRDRIScreen *psPVRScreen = psPVRContext->psPVRScreen;

	PVRDRIScreenLock(psPVRScreen);

	PVRContextUnbind(psPVRContext, false, false);

	PVRDRIDestroyContextImpl(psPVRContext->psImpl,
				 psPVRContext->eAPI,
				 psPVRScreen->psImpl);

	free(psPVRContext);

	PVRDRIScreenUnlock(psPVRScreen);

	PVRDRIScreenRemoveReference(psPVRScreen);
}

static IMG_PIXFMT PVRDRIGetPixelFormat(const struct gl_config *psGLMode)
{
	switch (psGLMode->rgbBits)
	{
		case 32:
		case 24:
			if (psGLMode->redMask   == 0x00FF0000 &&
			    psGLMode->greenMask == 0x0000FF00 &&
			    psGLMode->blueMask  == 0x000000FF)
			{
				if (psGLMode->alphaMask == 0xFF000000)
				{
					return IMG_PIXFMT_B8G8R8A8_UNORM;
				}
				else if (psGLMode->alphaMask == 0)
				{
					return IMG_PIXFMT_B8G8R8X8_UNORM;
				}
			}

			if (psGLMode->redMask   == 0x000000FF &&
			    psGLMode->greenMask == 0x0000FF00 &&
			    psGLMode->blueMask  == 0x00FF0000)
			{
				if (psGLMode->alphaMask == 0xFF000000)
				{
					return IMG_PIXFMT_R8G8B8A8_UNORM;
				}
				else if (psGLMode->alphaMask == 0)
				{
					return IMG_PIXFMT_R8G8B8X8_UNORM;
				}
			}

			__driUtilMessage("%s: Unsupported buffer format", __func__);
			return IMG_PIXFMT_UNKNOWN;

		case 16:
			if (psGLMode->redMask   == 0xF800 &&
			    psGLMode->greenMask == 0x07E0 &&
			    psGLMode->blueMask  == 0x001F)
			{
				return IMG_PIXFMT_B5G6R5_UNORM;
			}

		default:
			errorMessage("%s: Unsupported screen format\n", __func__);
			return IMG_PIXFMT_UNKNOWN;
	}
}

static GLboolean PVRDRICreateBuffer(__DRIscreen *psDRIScreen,
                                    __DRIdrawable *psDRIDrawable,
                                    const struct gl_config *psGLMode,
                                    GLboolean bIsPixmap)
{
	PVRDRIScreen *psPVRScreen = DRIScreenPrivate(psDRIScreen);
	PVRDRIDrawable *psPVRDrawable = NULL;
	PVRDRIDrawableImpl *psDrawableImpl = NULL;
	EGLint supportedAPIs = PVRDRIScreenSupportedAPIs(psPVRScreen);
	PVRDRIConfigInfo sConfigInfo;

	/* No known callers ever set this to true */
	if (bIsPixmap)
	{
		return GL_FALSE;
	}

	if (!psGLMode)
	{
		__driUtilMessage("%s: Invalid GL config", __func__);
		return GL_FALSE;
	}

	psPVRDrawable = calloc(1, sizeof(*psPVRDrawable));
	if (!psPVRDrawable)
	{
		__driUtilMessage("%s: Couldn't allocate PVR drawable", __func__);
		goto ErrorDrawableFree;
	}

	psDrawableImpl = PVRDRICreateDrawableImpl(psPVRDrawable);
	if (!psDrawableImpl)
	{
		__driUtilMessage("%s: Couldn't allocate PVR drawable", __func__);
		goto ErrorDrawableFree;
	}

	psPVRDrawable->psImpl = psDrawableImpl;

	psDRIDrawable->driverPrivate = (void *)psPVRDrawable;

	INITIALISE_QUEUE_HEAD(&psPVRDrawable->sPVRContextHead);

	psPVRDrawable->psDRIDrawable = psDRIDrawable;
	psPVRDrawable->psPVRScreen = psPVRScreen;
	psPVRDrawable->bDoubleBuffered = psGLMode->doubleBufferMode;

	psPVRDrawable->ePixelFormat = PVRDRIGetPixelFormat(psGLMode);
	if (psPVRDrawable->ePixelFormat == IMG_PIXFMT_UNKNOWN)
	{
		__driUtilMessage("%s: Couldn't work out pixel format", __func__);
		goto ErrorDrawableFree;
	}

	if (!PVRMutexInit(&psPVRDrawable->sMutex, PTHREAD_MUTEX_RECURSIVE))
	{
		__driUtilMessage("%s: Couldn't initialise drawable mutex", __func__);
		goto ErrorDrawableFree;
	}

	PVRDRIConfigFromMesa(&sConfigInfo, psGLMode);
	if (!PVRDRIEGLDrawableConfigFromGLMode(psDrawableImpl,
	                                       &sConfigInfo,
	                                       supportedAPIs,
	                                       psPVRDrawable->ePixelFormat))
	{
		__driUtilMessage("%s: Couldn't derive EGL config", __func__);
		goto ErrorDrawableMutexDeinit;
	}

	/* Initialisation is completed in MakeCurrent */
	PVRDRIScreenAddReference(psPVRScreen);
	return GL_TRUE;

ErrorDrawableMutexDeinit:
	PVRMutexDeinit(&psPVRDrawable->sMutex);

ErrorDrawableFree:
	PVRDRIDestroyDrawableImpl(psDrawableImpl);
	free(psPVRDrawable);
	psDRIDrawable->driverPrivate = NULL;

	return GL_FALSE;
}

static void PVRDRIDestroyBuffer(__DRIdrawable *psDRIDrawable)
{
	PVRDRIDrawable *psPVRDrawable = (PVRDRIDrawable *)psDRIDrawable->driverPrivate;
	PVRDRIScreen *psPVRScreen = psPVRDrawable->psPVRScreen;

	PVRDRIScreenLock(psPVRScreen);

	PVRDrawableUnbindContexts(psPVRDrawable);

	PVRDRIDrawableDeinit(psPVRDrawable);

	PVREGLDrawableDestroyConfig(psPVRDrawable->psImpl);

	PVRMutexDeinit(&psPVRDrawable->sMutex);

	PVRDRIDestroyDrawableImpl(psPVRDrawable->psImpl);

	free(psPVRDrawable);

	PVRDRIScreenUnlock(psPVRScreen);

	PVRDRIScreenRemoveReference(psPVRScreen);
}

static GLboolean PVRDRIMakeCurrent(__DRIcontext *psDRIContext,
				   __DRIdrawable *psDRIWrite,
				   __DRIdrawable *psDRIRead)
{
	PVRDRIContext *psPVRContext = (PVRDRIContext *)psDRIContext->driverPrivate;
	PVRDRIDrawable *psPVRWrite = (psDRIWrite) ? (PVRDRIDrawable *)psDRIWrite->driverPrivate : NULL;
	PVRDRIDrawable *psPVRRead = (psDRIRead) ? (PVRDRIDrawable *)psDRIRead->driverPrivate : NULL;

	PVRDRIScreenLock(psPVRContext->psPVRScreen);

	if (psPVRWrite != NULL)
	{
		if (!PVRDRIDrawableInit(psPVRWrite))
		{
			__driUtilMessage("%s: Couldn't initialise write drawable",
					 __func__);
			goto ErrorUnlock;
		}
	}

	if (psPVRRead != NULL)
	{
		if (!PVRDRIDrawableInit(psPVRRead))
		{
			__driUtilMessage("%s: Couldn't initialise read drawable",
					 __func__);
			goto ErrorUnlock;
		}
	}

	if (!PVRDRIMakeCurrentGC(psPVRContext->eAPI,
	                         psPVRContext->psPVRScreen->psImpl,
	                         psPVRContext->psImpl,
	                         psPVRWrite == NULL ? NULL : psPVRWrite->psImpl,
	                         psPVRRead  == NULL ? NULL : psPVRRead->psImpl))
	{
		goto ErrorUnlock;
	}

	queue_dequeue(&psPVRContext->sQElem);

	if (psPVRWrite != NULL)
	{
		queue_enqueue(&psPVRWrite->sPVRContextHead, &psPVRContext->sQElem);
	}

	psPVRContext->psPVRDrawable = psPVRWrite;

	if (psPVRWrite != NULL && psPVRContext->eAPI == PVRDRI_API_GL)
	{
		PVRDRIEGLSetFrontBufferCallback(psPVRContext->eAPI,
		                                psPVRContext->psPVRScreen->psImpl,
		                                psPVRWrite->psImpl,
		                                PVRDRIDisplayFrontBuffer);
	}

	pvrdri_set_dispatch_table(psPVRContext);

	PVRDRIThreadSetCurrentScreen(psPVRContext->psPVRScreen);

	PVRDRIScreenUnlock(psPVRContext->psPVRScreen);

	return GL_TRUE;

ErrorUnlock:
	PVRDRIScreenUnlock(psPVRContext->psPVRScreen);

	return GL_FALSE;
}

static GLboolean PVRDRIUnbindContext(__DRIcontext *psDRIContext)
{
	PVRDRIContext *psPVRContext = (PVRDRIContext *)psDRIContext->driverPrivate;
	PVRDRIScreen *psPVRScreen = psPVRContext->psPVRScreen;

	pvrdri_set_null_dispatch_table();

	PVRDRIScreenLock(psPVRScreen);
	PVRContextUnbind(psPVRContext, true, false);
	PVRDRIThreadSetCurrentScreen(NULL);
	PVRDRIScreenUnlock(psPVRScreen);

	return GL_TRUE;
}

static __DRIbuffer *PVRDRIAllocateBuffer(__DRIscreen *psDRIScreen,
					 unsigned int uAttachment,
					 unsigned int uFormat,
					 int iWidth,
					 int iHeight)
{
	PVRDRIScreen *psPVRScreen = DRIScreenPrivate(psDRIScreen);
	PVRBuffer *psBuffer;
	unsigned int uiBpp;

	/* GEM names are only supported on primary nodes */
	if (drmGetNodeTypeFromFd(psDRIScreen->fd) != DRM_NODE_PRIMARY)
	{
		__driUtilMessage("%s: Cannot allocate buffer", __func__);
		return NULL;
	}

	/* This is based upon PVRDRIGetPixelFormat */
	switch (uFormat)
	{
		case 32:
		case 16:
			/* Format (depth) and bpp match */
			uiBpp = uFormat;
			break;
		case 24:
			uiBpp = 32;
			break;
		default:
			__driUtilMessage("%s: Unsupported format '%u'",
					 __func__, uFormat);
			return NULL;
	}

	psBuffer = calloc(1, sizeof(*psBuffer));
	if (psBuffer == NULL)
	{
		__driUtilMessage("%s: Failed to allocate buffer", __func__);
		return NULL;
	}

	psBuffer->psImpl = PVRDRIBufferCreate(psPVRScreen->psImpl,
					      iWidth,
					      iHeight,
					      uiBpp,
					      PVDRI_BUFFER_USE_SHARE,
					      &psBuffer->sDRIBuffer.pitch);
	if (!psBuffer->psImpl)
	{
		__driUtilMessage("%s: Failed to create backing buffer",
				 __func__);
		goto ErrorFreeDRIBuffer;
	}

	psBuffer->sDRIBuffer.attachment = uAttachment;
	psBuffer->sDRIBuffer.name = PVRDRIBufferGetName(psBuffer->psImpl);
	psBuffer->sDRIBuffer.cpp = uiBpp / 8;

	return &psBuffer->sDRIBuffer;

ErrorFreeDRIBuffer:
	free(psBuffer);

	return NULL;
}

static void PVRDRIReleaseBuffer(__DRIscreen *psDRIScreen,
				__DRIbuffer *psDRIBuffer)
{
	PVRBuffer *psBuffer = (PVRBuffer *)psDRIBuffer;

	(void)psDRIScreen;

	PVRDRIBufferDestroy(psBuffer->psImpl);
	free(psBuffer);
}

/* Publish our driver implementation to the world. */
static const struct __DriverAPIRec pvr_driver_api =
{
	.InitScreen     = PVRDRIInitScreen,
	.DestroyScreen  = PVRDRIDestroyScreen,
	.CreateContext  = PVRDRICreateContext,
	.DestroyContext = PVRDRIDestroyContext,
	.CreateBuffer   = PVRDRICreateBuffer,
	.DestroyBuffer  = PVRDRIDestroyBuffer,
	.SwapBuffers    = NULL,
	.MakeCurrent    = PVRDRIMakeCurrent,
	.UnbindContext  = PVRDRIUnbindContext,
	.AllocateBuffer = PVRDRIAllocateBuffer,
	.ReleaseBuffer  = PVRDRIReleaseBuffer,
};

static const struct __DRIDriverVtableExtensionRec pvr_vtable = {
   .base = { __DRI_DRIVER_VTABLE, 1 },
   .vtable = &pvr_driver_api,
};

static const __DRIextension *pvr_driver_extensions[] = {
    &driCoreExtension.base,
    &driImageDriverExtension.base,
    &driDRI2Extension.base,
    &pvr_vtable.base,
    NULL
};

const __DRIextension **__driDriverGetExtensions_pvr(void);
PUBLIC const __DRIextension **__driDriverGetExtensions_pvr(void)
{
   globalDriverAPI = &pvr_driver_api;

   return pvr_driver_extensions;
}

/*************************************************************************/ /*!
 Global functions
*/ /**************************************************************************/

/***********************************************************************************
 Function Name	: PVRDRIDrawableLock
 Inputs		: psPVRDrawable - PVRDRI drawable structure
 Returns	: Boolean
 Description	: Lock drawable mutex (can be called recursively)
************************************************************************************/
void PVRDRIDrawableLock(PVRDRIDrawable *psPVRDrawable)
{
	int res;

	res  = pthread_mutex_lock(&psPVRDrawable->sMutex);
	if (res != 0)
	{
		errorMessage("%s: Failed to lock drawable (%d)\n", __func__, res);
		abort();
	}
}

/***********************************************************************************
 Function Name	: PVRDRIDrawableUnlock
 Inputs		: psPVRDrawable - PVRDRI drawable structure
 Returns	: Boolean
 Description	: Unlock drawable mutex (can be called recursively)
************************************************************************************/
void PVRDRIDrawableUnlock(PVRDRIDrawable *psPVRDrawable)
{
	int res;

	res  = pthread_mutex_unlock(&psPVRDrawable->sMutex);
	if (res != 0)
	{
		errorMessage("%s: Failed to unlock drawable (%d)\n", __func__, res);
		abort();
	}
}

/***********************************************************************************
 Function Name	: PVRDRIScreenLock
 Inputs		: psPVRScreen - PVRDRI screen structure
 Returns	: Boolean
 Description	: Lock screen mutex (can be called recursively)
************************************************************************************/
void PVRDRIScreenLock(PVRDRIScreen *psPVRScreen)
{
	int res;

	res  = pthread_mutex_lock(&psPVRScreen->sMutex);
	if (res != 0)
	{
		errorMessage("%s: Failed to lock screen (%d)\n", __func__, res);
		abort();
	}
}

/***********************************************************************************
 Function Name	: PVRDRIScreenUnlock
 Inputs		: psPVRScreen - PVRDRI screen structure
 Returns	: Boolean
 Description	: Unlock screen mutex (can be called recursively)
************************************************************************************/
void PVRDRIScreenUnlock(PVRDRIScreen *psPVRScreen)
{
	int res;

	res  = pthread_mutex_unlock(&psPVRScreen->sMutex);
	if (res != 0)
	{
		errorMessage("%s: Failed to unlock screen (%d)\n", __func__, res);
		abort();
	}
}

void PVRDRIThreadSetCurrentScreen(PVRDRIScreen *psPVRScreen)
{
	gpsPVRScreen = psPVRScreen;
}

PVRDRIScreen *PVRDRIThreadGetCurrentScreen(void)
{
	return gpsPVRScreen;
}
