/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, under
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

//// Dependencies

#include "cVKDriver.h"
#include "Logger.h"
#include "VulkanBackend.h"

//// Exports

// Both interfaces declare a pure virtual destructor, which still requires a definition
// for the derived destructor chain to link.
cIGZGBufferRegionExtension::~cIGZGBufferRegionExtension(void) { }
cIGZGDriverVertexBufferExtension::~cIGZGDriverVertexBufferExtension(void) { }

namespace scvk
{
	//// Private Functions

	void cVKDriver::SetLastError(DriverError error)
	{
		// Report a new error
		//
		// Worth reporting, because the game polls GetError after initialisation and
		// treats a non-zero result as the driver having failed: a stray error set deep in
		// some unrelated method is enough to get the whole driver rejected. The trace
		// line immediately above identifies who set it.
		//
		// Only on a change, though. The same error arriving repeatedly is one piece of
		// news, not many, and logging every occurrence produced 899,697 identical lines
		// the first time a blit failed once per call. A scoped enumeration is not
		// promoted when passed to a variadic function, so it is converted to the error
		// number it stands for.
		if (error != DriverError::OK && error != lastError)
		{
			LogNote("  !! error state set to %u (repeats suppressed until it changes)", static_cast<uint32_t>(error));
		}

		lastError = error;
	}

	//// Public API

	cVKDriver::cVKDriver(void) : vulkan(std::make_unique<VulkanBackend>())
	{
		refCount = 0;

		LogOpen();
		LogNote("cVKDriver constructed.");
	}

	cVKDriver::~cVKDriver(void)
	{
		LogNote("cVKDriver destroyed (refcount reached zero).");
	}

	bool cVKDriver::FactoryFunction(uint32_t interfaceId, void** outInterface)
	{
		cVKDriver* const driver = new cVKDriver();

		bool const hasInterface = driver->QueryInterface(interfaceId, outInterface);
		if (!hasInterface || *outInterface == nullptr)
		{
			delete driver;
			return false;
		}

		return true;
	}

	bool cVKDriver::QueryInterface(uint32_t interfaceId, void** outInterface)
	{
		// Hand out the interface asked for
		//
		// Each is a base of this class, so the conversion picks the matching subobject.
		switch (interfaceId)
		{
		case GZIID_cIGZUnknown:
		case GZIID_cIGZGDriver:
			LogNote("QueryInterface(%08x) -> cIGZGDriver", interfaceId);
			*outInterface = static_cast<cIGZGDriver*>(this);
			break;

		case GZIID_cIGZGBufferRegionExtension:
			LogNote("QueryInterface(%08x) -> cIGZGBufferRegionExtension", interfaceId);
			*outInterface = static_cast<cIGZGBufferRegionExtension*>(this);
			break;

		case GZIID_cIGZGDriverLightingExtension:
			LogNote("QueryInterface(%08x) -> cIGZGDriverLightingExtension", interfaceId);
			*outInterface = static_cast<cIGZGDriverLightingExtension*>(this);
			break;

		case GZIID_cIGZGSnapshotExtension:
			// Documented as mandatory: refusing this one crashes the game during load,
			// even though it is nominally an extension.
			LogNote("QueryInterface(%08x) -> cIGZGSnapshotExtension", interfaceId);
			*outInterface = static_cast<cIGZGSnapshotExtension*>(this);
			break;

		case GZIID_cIGZGDriverVertexBufferExtension:
			// Deliberately declined. Accepting it opts the game into a buffer-object draw
			// path, and the client-memory path is the one scvk implements. SCGL also
			// leaves this one disabled. Logged so the boot trace still records that the
			// game asked.
			LogNote("QueryInterface(%08x) -> cIGZGDriverVertexBufferExtension DECLINED (stub stage)", interfaceId);
			return false;

		default:
			LogNote("QueryInterface(%08x) -> unrecognised, declined", interfaceId);
			return false;
		}

		AddRef();
		return true;
	}

	uint32_t cVKDriver::AddRef(void)
	{
		return cRZRefCount::AddRef();
	}

	uint32_t cVKDriver::Release(void)
	{
		return cRZRefCount::Release();
	}

	bool cVKDriver::FinalRelease(void)
	{
		SCVK_CALL("");
		return true;
	}

	uint32_t cVKDriver::GetError(void)
	{
		// Read and clear, matching the GL error semantics the interface is modelled on.
		// The enumeration's underlying type is the interface's error number.
		uint32_t const error = static_cast<uint32_t>(lastError);
		lastError = DriverError::OK;

		SCVK_CALL("");
		LogNote("  -> returned error %u%s", error, error == 0 ? " (OK)" : "  <-- the game reads this as failure");
		return error;
	}

	char const* cVKDriver::GetDriverInfo(void) const
	{
		SCVK_CALL("");
		return driverInformation.c_str();
	}

	uint32_t cVKDriver::GetGZCLSID(void) const
	{
		SCVK_CALL("");
		return DRIVER_GZCLSID;
	}

	bool cVKDriver::Punt(uint32_t unknown, void* unknown2)
	{
		SCVK_CALL("%u, %p", unknown, unknown2);
		return false;
	}
}
