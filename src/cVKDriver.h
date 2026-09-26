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

#pragma once

//// Dependencies

#include <memory>
#include <string>
#include <vector>

#include <cIGZGDriver.h>
#include <cRZRefCount.h>
#include <sGDMode.h>

#include <ext/cIGZGBufferRegionExtension.h>
#include <ext/cIGZGDriverLightingExtension.h>
#include <ext/cIGZGDriverVertexBufferExtension.h>
#include <ext/cIGZGSnapshotExtension.h>

namespace scvk
{
	//// Types

	// Held by pointer so vulkan.h, and the windows.h it drags in, stay out of every
	// translation unit that only needs the driver interface.
	class VulkanBackend;

	/**
	 * scvk's implementation of SimCity 4's renderer interface.
	 *
	 * The interface is shaped like OpenGL 1.2 with extensions: a fixed function pipeline,
	 * immediate-mode draws, a matrix stack, and texture environment combiners. None of
	 * that exists in Vulkan, so this tracks the state the game sets and hands it to the
	 * backend, which emulates the fixed function pipeline with pipelines keyed on that
	 * state and an ubershader driven by push constants.
	 *
	 * Every method the game calls is traced to scvk.log, which is how the interface was
	 * mapped in the first place: which of its ~117 methods the game actually calls, in
	 * what order, and how often.
	 *
	 * The source is split by concern: cVKDriver.cpp holds the COM plumbing, and the
	 * cVKDriver_*.cpp files hold video modes and frames, render state, draws, textures,
	 * blits, the extensions, and the diagnostics.
	 */
	class cVKDriver final :
		public cIGZGDriver,
		public cIGZGBufferRegionExtension,
		public cIGZGDriverLightingExtension,
		public cIGZGDriverVertexBufferExtension,
		public cIGZGSnapshotExtension,
		public cRZRefCount
	{
	private:
		//// Types

		/** Mirrors the error codes the game's own driver reports. */
		enum class DriverError : uint32_t
		{
			OK                    = 0,
			OUT_OF_RANGE          = 2,
			NOT_SUPPORTED         = 3,
			CREATE_CONTEXT_FAILED = 6,
			INVALID_ENUM          = 0x500,
			INVALID_VALUE         = 0x501,
		};

		/** Draws of a saved tile that share the state deciding whether they reach the screen. */
		struct TileClass
		{
			uint32_t key;
			uint32_t count;
			uint32_t firstTexture;
			float    minimumDepth;
			float    maximumDepth;
		};

		/** One partial update the game saved: the rectangle, and what was drawn into it. */
		struct TileRecord
		{
			static constexpr int CLASS_LIMIT = 24;

			uint32_t  frame;
			int32_t   saveRectangle[4];
			int32_t   subViewport[4];
			uint32_t  subViewportDraws;
			uint32_t  fullViewportDraws;
			uint32_t  unclassifiedDraws;
			uint64_t  hazards;
			int       classCount;
			TileClass classes[CLASS_LIMIT];

			// The draw record sequence numbers the tile's draws ran from and up to.
			uint32_t firstDraw;
			uint32_t endDraw;
		};

		/**
		 * One draw of a saved tile, written out on Scroll Lock as raw bytes.
		 *
		 * Fixed-width fields only, so tools/draw-records.py can read the file with one
		 * struct format. Bounds are window pixels from the top-left, and depth is the
		 * window depth OpenGL would compute.
		 */
		struct DrawRecord
		{
			uint32_t sequence;
			uint32_t frame;
			uint32_t vertexFormat;
			uint32_t primitiveType;
			uint32_t count;
			uint32_t textures[2];
			uint32_t flags;
			uint8_t  blendSource;
			uint8_t  blendDestination;
			uint8_t  depthComparison;
			uint8_t  alphaComparison;
			uint8_t  environmentModes[2];
			uint8_t  textureLevels;
			uint8_t  textureUploadedLevels;
			float    alphaReference;
			float    tint[4];
			float    diffuseLight;
			uint8_t  colourMinimum[4];
			uint8_t  colourMaximum[4];
			float    bounds[4];
			float    depthRange[2];
			float    coordinateRange[4];
			uint16_t textureWidth;
			uint16_t textureHeight;
			uint32_t textureUploads;
			uint32_t vertexAddress;
			uint32_t lowestVertex;
			uint32_t highestVertex;
			uint32_t geometryHash;

			// The first two rows of the first stage's texture matrix.
			float textureMatrixRows[8];

			// The filter and wrap the first stage's texture samples with, and the stage
			// the game had selected.
			uint8_t samplerParameters[4];
			uint8_t activeTextureStage;
			uint8_t coordinateSources[2];

			// Keeps the record a multiple of four bytes.
			uint8_t padding;
		};

	public:
		//// Constants

		/**
		 * The game's own OpenGL driver class ID.
		 *
		 * SimCity 4 picks a renderer by class ID and has exactly three slots: DirectX,
		 * OpenGL and Software. There is no room to register a fourth, so scvk claims the
		 * OpenGL slot and outranks the built-in driver by reporting a higher version from
		 * EnumClassObjects. SCGL does the same, which is why scvk and SCGL cannot be
		 * installed at the same time.
		 */
		static constexpr uint32_t DRIVER_GZCLSID = 0xC4554841;

		/** Reported to the GZCOM so our registration beats the built-in driver. */
		static constexpr uint32_t DRIVER_VERSION = 1000000;

	private:
		// A dump stays open over a window of frames, because the frame worth seeing is
		// the sparse one that rebuilds the scene rather than any of the many that just
		// restore it from a buffer region.
		static constexpr int DUMP_WINDOW_FRAMES   = 240;
		static constexpr int MAXIMUM_DUMPED_DRAWS = 4000;

		// The partial update trace. Region copies and clears are buffered per frame, with
		// the draws and sub-viewport between them, and written out only for frames that
		// save part of the scene. Every city frame restores the whole scene, so logging
		// all of them would bury the few that matter.
		static constexpr int REGION_TRACE_FRAMES    = 40;
		static constexpr int REGION_LINES_PER_FRAME = 400;
		static constexpr int REGION_LINE_LENGTH     = 200;
		static constexpr int REGION_PENDING_DRAWS   = 40;

		// Saved tiles kept for Scroll Lock to write out.
		static constexpr uint32_t TILE_RING_SIZE = 256;

		// Draws kept for Scroll Lock to write out, enough for the four tiles of a full
		// rebuild and the small ones after it.
		static constexpr uint32_t DRAW_RING_SIZE = 131072;

		// The bits of DrawRecord::flags.
		static constexpr uint32_t DRAW_FLAG_STAGE0          = 1u << 0;
		static constexpr uint32_t DRAW_FLAG_STAGE1          = 1u << 1;
		static constexpr uint32_t DRAW_FLAG_BLEND           = 1u << 2;
		static constexpr uint32_t DRAW_FLAG_DEPTH_TEST      = 1u << 3;
		static constexpr uint32_t DRAW_FLAG_DEPTH_WRITE     = 1u << 4;
		static constexpr uint32_t DRAW_FLAG_COLOUR_WRITE    = 1u << 5;
		static constexpr uint32_t DRAW_FLAG_ALPHA_TEST      = 1u << 6;
		static constexpr uint32_t DRAW_FLAG_GENERATED       = 1u << 7;
		static constexpr uint32_t DRAW_FLAG_AMBIENT_VERTEX  = 1u << 8;
		static constexpr uint32_t DRAW_FLAG_DIFFUSE_VERTEX  = 1u << 9;
		static constexpr uint32_t DRAW_FLAG_INDEXED         = 1u << 10;
		static constexpr uint32_t DRAW_FLAG_TEXTURE_LIVE    = 1u << 11;
		static constexpr uint32_t DRAW_FLAG_BEHIND_CAMERA   = 1u << 12;
		static constexpr uint32_t DRAW_FLAG_VERTICES_CAPPED = 1u << 13;

		// The kinds of configuration NoteOnce reports, each keyed separately.
		static constexpr uint32_t NOTE_COMBINER            = 1;
		static constexpr uint32_t NOTE_STAGE_BINDING       = 2;
		static constexpr uint32_t NOTE_TEXTURE_ENVIRONMENT = 3;
		static constexpr uint32_t NOTE_VERTEX_FORMAT       = 4;
		static constexpr uint32_t NOTE_MULTITEXTURE        = 5;
		static constexpr uint32_t NOTE_TEXTURE_PARAMETER   = 6;
		static constexpr uint32_t NOTE_STAGE1_TEXTURE      = 7;
		static constexpr uint32_t NOTE_SCENE_TINT          = 8;
		static constexpr uint32_t NOTE_CLOUD_SHADOW        = 9;
		static constexpr uint32_t NOTE_COORDINATE_SOURCE   = 10;
		static constexpr uint32_t NOTE_TERRAIN_TEXTURE     = 11;

		//// State

		// The window and its video modes
		DriverError                    lastError        = DriverError::OK;
		std::vector<sGDMode>           videoModes;
		int                            currentVideoMode = -1;
		std::string                    driverInformation;
		int                            windowWidth      = 0;
		int                            windowHeight     = 0;
		void*                          windowHandle     = nullptr;
		std::unique_ptr<VulkanBackend> vulkan;

		// The viewport, in the game's bottom-origin coordinates
		int viewportX      = 0;
		int viewportY      = 0;
		int viewportWidth  = 0;
		int viewportHeight = 0;

		// Tracked rather than stubbed: the game reads these back through IsEnabled and
		// branches on the answer, so returning a constant would distort the call
		// sequence.
		bool isCapabilityEnabled[kGDNumCapabilities] = {};

		// Handed out by GenTextures. The game stores these and passes them back, and
		// treats zero as "no texture", so names must be unique and non-zero.
		uint32_t nextTextureName = 1;

		// The texture on each stage, as backend handles.
		uint32_t boundTexture  = 0;
		uint32_t stage1Texture = 0;

		// Which stage the stage-scoped calls refer to, and whether texturing is switched
		// on for each. Both are per stage in this interface even though the enable
		// arrives through the same call as the global capabilities.
		uint32_t activeTextureStage       = 0;
		bool     isTextureStageEnabled[2] = { false, false };

		// Which coordinate source each stage was last told to sample with.
		uint32_t textureCoordinateSource[2] = { 0, 1 };

		// Each stage's texture matrix, which transforms its coordinates whether they come
		// from the vertex or are generated.
		float textureStageMatrices[2][16] = {
			{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 },
			{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 },
		};

		// What each stage was last told to combine with, and how. The mode decides which
		// of the two the stage actually uses: the network only applies when the mode
		// selects Combine, which setting a network does.
		int32_t  textureEnvironmentMode[2] = { kGDTextureEnvParam_Modulate, kGDTextureEnvParam_Modulate };
		uint32_t rawCombiner[4]            = {};

		// Each stage's environment colour, which a combiner may name as a source.
		float environmentColours[2][4] = {};

		// The combiner network as the shader will read it, kept so a draw can report the
		// configuration that was actually in force for it.
		uint32_t packedCombiner[4] = {};

		// Held from ClearColor and ClearDepth until the next Clear, because the interface
		// splits what Vulkan takes as a single call.
		float clearColour[4]  = { 0.0f, 0.0f, 0.0f, 1.0f };
		float clearDepthValue = 1.0f;

		// Blend, alpha test, depth and colour write state, in the game's own
		// enumerations. All of it is pipeline or shader state in Vulkan rather than
		// commands, so it is forwarded on change rather than applied immediately.
		uint32_t blendSourceFactor      = 1;
		uint32_t blendDestinationFactor = 0;
		uint32_t alphaComparison        = 7;
		float    alphaReference         = 0.0f;
		uint32_t depthComparison        = 1;
		bool     isDepthWriteEnabled    = true;
		bool     isColourWriteEnabled   = true;

		// The fixed function matrix stack, reduced to what the game uses: it only ever
		// loads whole matrices into the modelview and projection slots, never pushes,
		// pops or multiplies.
		float    modelViewMatrix[16]  = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		float    projectionMatrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		uint32_t activeMatrix         = 0;

		// The global ambient light colour and the diffuse material alpha, plus whether
		// the vertex colour feeds either. Together these are all the lighting SimCity 4
		// uses.
		float colourMultiplier[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool  isVertexColourAmbient = false;
		bool  isVertexColourDiffuse = false;

		// The diffuse light term for the current modelview. The light is directional and
		// fixed, the geometry carries no normals, so this is one number per transform
		// rather than per vertex.
		float diffuseLightFactor = 0.0f;

		// The last format handed to InterleavedArrays, and the client pointer it named.
		// Draws read from that pointer, so the driver has to keep both until the draw
		// arrives.
		uint32_t    vertexFormat  = 0;
		uint32_t    vertexStride  = 0;
		void const* vertexPointer = nullptr;

		// Set by the scvk-skip-cloud-shadows marker file, to see the picture without that
		// pass.
		bool shouldSkipCloudShadows = false;

		// Configurations already reported once, and the last multitextured one, so an
		// unchanged one costs a compare rather than a search.
		uint64_t notedKeys[192]      = {};
		uint32_t notedCount          = 0;
		uint32_t lastMultitextureKey = 0xffffffffu;

		// Diagnostic budgets. The geometry probe samples one draw per distinct vertex
		// format, primitive type and projection rather than by position in the frame, so
		// each rendering context gets described once. Sampling the first few draws
		// instead reported only tiny sub-pixel quads, which said nothing about what was
		// actually on screen.
		int      blitProbesRemaining          = 3;
		uint32_t probedKeys[48]               = {};
		uint32_t probedCombinations           = 0;
		int      textureMatrixProbesRemaining = 4;
		int      mismatchReportsRemaining     = 12;
		int      coverageReportsRemaining     = 12;
		int      indexTypeWarningsRemaining   = 4;
		uint32_t lastTextureMatrixFlags       = 0;

		// The frame dump. A dump waits for the terrain pass rather than starting on a
		// frame boundary. Most frames restore the scene from a buffer region and contain
		// nothing but interface, and those were filling the budget before a redraw ever
		// arrived.
		uint32_t frameCounter        = 0;
		bool     isDumpingFrame      = false;
		bool     isDumpArmed         = false;
		int      dumpedDraws         = 0;
		int      frameDumpsRemaining = 10;
		int      dumpWindowRemaining = 0;

		// Scroll Lock captures the screen, the saved scene and the saved depth on three
		// consecutive frames. The step counts down the ones still to take.
		int      keyCaptureStep   = 0;
		uint32_t keyCaptureCount  = 0;
		bool     isCaptureKeyHeld = false;

		// The partial update trace: the frame's steps, whether the frame is worth writing
		// out, and the draws since the last step.
		char     regionLines[REGION_LINES_PER_FRAME][REGION_LINE_LENGTH] = {};
		int      regionLineCount          = 0;
		int      regionLinesDropped       = 0;
		int      regionTraceFrames        = REGION_TRACE_FRAMES;
		bool     isRegionFrameInteresting = false;
		bool     hasRegionFrameRestored   = false;
		uint32_t regionDrawsSinceStep     = 0;

		// The first draws under the current sub-viewport, attached to the next save so a
		// partial update shows what it drew. A new sub-viewport discards them, which
		// keeps the interface's many small ones out.
		char regionPending[REGION_PENDING_DRAWS][REGION_LINE_LENGTH] = {};
		int  regionPendingCount = 0;
		int  regionPendingTotal = 0;

		// The last sub-viewport set since the previous step, zero width when none was.
		int regionSubViewport[4] = {};

		// Every partial update the game saves, summarised and kept in a ring so Scroll
		// Lock can write out the ones just before a capture. A tile is the draws since
		// the previous save, grouped by the state that decides whether they reach the
		// screen.
		TileRecord currentTile        = {};
		uint64_t   tileHazardsAtStart = 0;
		TileRecord tileRing[TILE_RING_SIZE] = {};
		uint32_t   tileRingNext       = 0;
		uint32_t   tileRingCount      = 0;

		// Every draw of the tiles, in a ring sized only when the scvk-record-tile-draws
		// marker is present, since it costs a transform of every vertex and about 23 MB.
		std::vector<DrawRecord> drawRing;
		uint32_t                drawSequence = 0;

		//// Private Functions

		// Plumbing and frames, in cVKDriver.cpp and cVKDriver_Video.cpp

		void     SetLastError(DriverError error);
		uint32_t EnumerateVideoModes(void);

		/** Fills in the description GetDriverInfo returns. */
		void BuildDriverInformation(void);

		/** Switches on the diagnostics whose marker files sit next to the driver. */
		void ApplyDiagnosticMarkers(void);

		/** Creates and shows the game's window for a mode, replacing any earlier one. */
		bool CreateRenderWindow(sGDMode const& mode, void* windowProcedure);

		void DestroyRenderWindow(void);

		// Render state, in cVKDriver_State.cpp

		/** Applies Enable or Disable to one capability. */
		void SetCapability(uint32_t gdCapability, bool isEnabled);

		/** Forwards blend enable and factors, which the backend keys on. */
		void PushBlendState(void);

		/** Forwards depth test, write and comparison. */
		void PushDepthState(void);

		/** Forwards the alpha comparison, disabled when the capability is off. */
		void PushAlphaTest(void);

		/** Forwards the tint, and logs the day and night cycle. */
		void PushSceneTint(void);

		/** Forwards the lighting weight and the alpha source. */
		void PushLighting(void);

		// Draws, in cVKDriver_Draw.cpp

		/** Recomputes projection times modelview and hands it to the backend. */
		void UpdateTransform(void);

		/** Recomputes and forwards where each stage's coordinates come from. */
		void PushStageCoordinates(void);

		/** Whether a stage generates its coordinates from the eye-space position. */
		bool IsGeneratingCoordinates(uint32_t stage) const;

		/** The cloud shadow pass, the only single stage pass that generates its coordinates. */
		bool IsCloudShadowDraw(void) const;

		// Textures, in cVKDriver_Textures.cpp

		/** Applies a texture enable to the stage TexStage last selected. */
		void SetTextureStageEnabled(bool isEnabled);

		/** Recomputes both stages from the mode and network and pushes them. */
		void PushCombinerState(void);

		// Blits, in cVKDriver_Blit.cpp

		/**
		 * Shared by all six blit entry points.
		 *
		 * They differ only in how they scale and how they treat alpha; the pixel upload
		 * underneath is identical, so it lives in one place.
		 */
		void UploadBlit(char const* caller, int32_t destinationLeft, int32_t destinationTop, int32_t destinationWidth, int32_t destinationHeight, int32_t sourceWidth, int32_t sourceHeight, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer1, void const* buffer2);

		// Diagnostics, in cVKDriver_Diagnostics.cpp

		/**
		 * Reports each distinct configuration once, and answers false after.
		 *
		 * The ordinary trace stops after a call budget, which the interface alone
		 * exhausts long before a city finishes loading. Anything that needs observing
		 * inside a city has to be recorded by value instead of by position, so it still
		 * reports the first time it is seen no matter how late that is.
		 */
		bool NoteOnce(uint32_t bucket, uint32_t key);

		/** Whether the viewport is anything other than the whole window. */
		bool IsSubViewport(void) const;

		/** The bytes of a draw's i-th vertex, through its index array when it has one. */
		uint8_t const* VertexAt(int32_t first, void const* indices, bool isIndex32Bit, int i) const;

		/** Transforms a vertex's position to clip space, the way the fixed function pipeline would. */
		void ProjectVertex(uint8_t const* vertex, float outClip[4]) const;

		/** Describes one draw per distinct format, primitive and projection. */
		void ProbeDrawArrays(uint32_t gdPrimitiveType, int32_t first, int32_t count);

		/** Reports a draw covering most of its viewport. */
		void ReportLargeDraw(uint32_t gdPrimitiveType, int32_t first, int32_t count);

		/** Reports a projection that disagrees with the viewport it is drawn into. */
		void ReportProjectionMismatch(uint32_t gdPrimitiveType, int32_t count);

		/** Reports the state of draws made under a near black ambient tint. */
		void NoteDarkTintedDraw(uint32_t gdPrimitiveType, int32_t count);

		/** Reports the combiner in force, once per distinct multitextured draw. */
		void NoteMultitexturedDraw(uint32_t gdVertexFormat);

		/** Arms the dump when the terrain pass starts. */
		void MaybeArmDump(void);

		/** Records one draw of a dumped frame, from either draw path. */
		void DumpDraw(uint32_t gdPrimitiveType, int32_t count, int32_t first, void const* indices, bool isIndex32Bit);

		/** Window depth range of a draw's first few vertices, as OpenGL would compute it. */
		void SampleWindowDepth(int32_t count, int32_t first, void const* indices, bool isIndex32Bit, float& outMinimumDepth, float& outMaximumDepth) const;

		/** Buffers one step, preceded by the draws since the last one. */
		void NoteRegionStep(char const* format, ...);

		/** Buffers the draw count line, if any draws happened since the last step. */
		void AppendRegionDrawCount(void);

		/** Moves the pending sub-viewport draws into the frame's steps. */
		void AttachRegionPending(void);

		/** Counts a draw, and records it while a sub-viewport is in force. */
		void NoteRegionDraw(uint32_t gdPrimitiveType, int32_t count, int32_t first, void const* indices, bool isIndex32Bit);

		/** Called for every sub-viewport the game sets. */
		void NoteRegionSubViewport(void);

		/** Writes out the frame's steps if it saved part of the scene. */
		void FlushRegionTrace(void);

		/** Whether a copy covers the whole window at the origin. */
		bool IsFullWindowCopy(int32_t x, int32_t y, int32_t width, int32_t height, int32_t screenX, int32_t screenY) const;

		/** Adds one draw to the tile being built. */
		void NoteTileDraw(int32_t count, int32_t first, void const* indices, bool isIndex32Bit);

		/** Closes the tile on a save of part of the scene. */
		void NoteTileSave(int32_t x, int32_t y, int32_t width, int32_t height);

		/** Starts a fresh tile, dropping whatever was gathered. */
		void ResetTile(void);

		/** Measures one draw of the tile being built and keeps it in the draw ring. */
		void RecordTileDraw(uint32_t gdPrimitiveType, int32_t count, int32_t first, void const* indices, bool isIndex32Bit);

		/** Writes the ring to the log, oldest first. */
		void DumpTileRing(void);

		/** Writes the tile ring and its draws to a file, for tools/draw-records.py. */
		void WriteDrawRecords(char const* path);

		/** Closes the frame's dump and trace, and takes any capture that is due. */
		void EndFrameDiagnostics(void);

		/** Asks for the periodic frame and saved scene captures. */
		void RequestPeriodicCaptures(void);

		/** Starts and advances the Scroll Lock capture. */
		void PollKeyCapture(void);

	public:
		//// Public API

		cVKDriver(void);
		virtual ~cVKDriver(void) override;

		static bool FactoryFunction(uint32_t interfaceId, void** outInterface);

		// cIGZUnknown

		virtual bool     QueryInterface(uint32_t interfaceId, void** outInterface) override;
		virtual uint32_t AddRef(void) override;
		virtual uint32_t Release(void) override;

		// cIGZGDriver

		virtual void DrawArrays(uint32_t gdPrimitiveType, int32_t first, int32_t count) override;
		virtual void DrawElements(uint32_t gdPrimitiveType, int32_t count, uint32_t gdType, void const* indices) override;
		virtual void InterleavedArrays(uint32_t gdVertexFormat, int32_t stride, void const* pointer) override;

		virtual uint32_t MakeVertexFormat(uint32_t count, intptr_t gdElementTypeList) override;
		virtual uint32_t MakeVertexFormat(uint32_t gdVertexFormat) override;
		virtual uint32_t VertexFormatStride(uint32_t gdVertexFormat) override;
		virtual uint32_t VertexFormatElementOffset(uint32_t gdVertexFormat, uint32_t gdElementType, uint32_t index) override;
		virtual uint32_t VertexFormatNumElements(uint32_t gdVertexFormat, uint32_t gdElementType) override;

		virtual void Clear(uint32_t mask) override;
		virtual void ClearColor(float red, float green, float blue, float alpha) override;
		virtual void ClearDepth(double depth) override;
		virtual void ClearStencil(int32_t stencil) override;

		virtual void ColorMask(bool isEnabled) override;
		virtual void DepthFunc(uint32_t gdComparison) override;
		virtual void DepthMask(bool isEnabled) override;

		virtual void StencilFunc(uint32_t gdComparison, int32_t reference, uint32_t mask) override;
		virtual void StencilMask(uint32_t mask) override;
		virtual void StencilOp(uint32_t gdStencilFailOperation, uint32_t gdDepthFailOperation, uint32_t gdPassOperation) override;

		virtual void BlendFunc(uint32_t gdSourceFactor, uint32_t gdDestinationFactor) override;
		virtual void AlphaFunc(uint32_t gdComparison, float reference) override;
		virtual void ShadeModel(uint32_t gdShade) override;

		virtual void BindTexture(uint32_t gdTextureTarget, uint32_t texture) override;
		virtual void TexImage2D(uint32_t gdTextureTarget, int32_t level, int32_t gdInternalTextureFormat, int32_t width, int32_t height, int32_t border, uint32_t gdTextureFormat, uint32_t gdType, void const* pixels) override;
		virtual void PixelStore(uint32_t gdParameter, int32_t parameter) override;

		virtual void TexEnv(uint32_t gdTextureEnvironmentTarget, uint32_t gdTextureEnvironmentParameterType, int32_t gdTextureEnvironmentMode) override;
		virtual void TexEnv(uint32_t gdTextureEnvironmentTarget, uint32_t gdTextureEnvironmentParameterType, float const* parameters) override;
		virtual void TexParameter(uint32_t gdTextureTarget, uint32_t gdTextureParameterType, int32_t gdTextureParameter) override;

		virtual void Fog(uint32_t gdFogParameterType, uint32_t gdFogParameter) override;
		virtual void Fog(uint32_t gdFogParameterType, float const* parameters) override;

		virtual void ColorMultiplier(float red, float green, float blue) override;
		virtual void AlphaMultiplier(float alpha) override;
		virtual void EnableVertexColors(bool shouldFeedAmbient, bool shouldFeedDiffuse) override;

		virtual void GenTextures(int32_t count, uint32_t* textures) override;
		virtual void DeleteTextures(int32_t count, uint32_t const* textures) override;
		virtual bool IsTexture(uint32_t texture) override;
		virtual void PrioritizeTextures(int32_t count, uint32_t const* textures, float const* priorities) override;
		virtual bool AreTexturesResident(int32_t count, uint32_t const* textures, bool* residences) override;

		virtual void MatrixMode(uint32_t gdMatrixTarget) override;
		virtual void LoadMatrix(float const* matrix) override;
		virtual void LoadIdentity(void) override;

		virtual void Flush(void) override;
		virtual void Enable(uint32_t gdCapability) override;
		virtual void Disable(uint32_t gdCapability) override;
		virtual bool IsEnabled(uint32_t gdCapability) override;

		virtual void     GetBoolean(uint32_t gdParameter, bool* outValues) override;
		virtual void     GetInteger(uint32_t gdParameter, int32_t* outValues) override;
		virtual void     GetFloat(uint32_t gdParameter, float* outValues) override;
		virtual uint32_t GetError(void) override;

		virtual void TexStage(uint32_t textureUnit) override;
		virtual void TexStageCoord(uint32_t gdTextureCoordinateSource) override;
		virtual void TexStageMatrix(float const* matrix, uint32_t unknown0, uint32_t unknown1, uint32_t gdTextureMatrixFlags) override;
		virtual void TexStageCombine(eGDTextureStageCombineParamType gdParameterType, eGDTextureStageCombineModeParam gdParameter) override;
		virtual void TexStageCombine(eGDTextureStageCombineSourceParamType gdParameterType, eGDTextureStageCombineSourceParam gdParameter) override;
		virtual void TexStageCombine(eGDTextureStageCombineOperandType gdParameterType, eGDBlend gdBlend) override;
		virtual void TexStageCombine(eGDTextureStageCombineScaleParamType gdParameterType, eGDTextureStageCombineScaleParam gdParameter) override;

		virtual void     SetTexture(uint32_t texture, uint32_t textureUnit) override;
		virtual intptr_t GetTexture(uint32_t textureUnit) override;
		virtual intptr_t CreateTexture(uint32_t gdInternalTextureFormat, uint32_t width, uint32_t height, uint32_t levels, uint32_t gdTextureHintFlags) override;
		virtual void     LoadTextureLevel(uint32_t texture, int32_t level, int32_t offsetX, int32_t offsetY, int32_t width, int32_t height, uint32_t gdTextureFormat, uint32_t gdType, uint32_t rowLength, void const* pixels) override;
		virtual void     SetCombiner(cGDCombiner const& combiner, uint32_t textureUnit) override;

		virtual uint32_t CountVideoModes(void) const override;
		virtual void     GetVideoModeInfo(uint32_t modeIndex, sGDMode& outMode) override;
		virtual void     GetVideoModeInfo(sGDMode& outMode) override;

		// The two trailing flags are of unknown meaning and are ignored. See the
		// definition: one of them looks like a visibility hint, but acting on it hides
		// the game's window.
		virtual void SetVideoMode(int32_t newModeIndex, void* windowProcedure, bool isUnknownFlag1Set, bool isUnknownFlag2Set) override;

		virtual void PolygonOffset(int32_t offset) override;

		virtual void BitBlt(int32_t destinationLeft, int32_t destinationTop, int32_t width, int32_t height, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, bool isUnknownFlagSet, void const* buffer2) override;
		virtual void StretchBlt(int32_t destinationLeft, int32_t destinationTop, int32_t destinationWidth, int32_t destinationHeight, int32_t sourceWidth, int32_t sourceHeight, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, bool isUnknownFlagSet, void const* buffer2) override;
		virtual void BitBltAlpha(int32_t destinationLeft, int32_t destinationTop, int32_t width, int32_t height, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, bool isUnknownFlagSet, void const* buffer2, uint32_t alpha) override;
		virtual void StretchBltAlpha(int32_t destinationLeft, int32_t destinationTop, int32_t destinationWidth, int32_t destinationHeight, int32_t sourceWidth, int32_t sourceHeight, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, bool isUnknownFlagSet, void const* buffer2, uint32_t alpha) override;
		virtual void BitBltAlphaModulate(int32_t destinationLeft, int32_t destinationTop, int32_t width, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, bool isUnknownFlagSet, void const* buffer2, uint32_t alpha) override;
		virtual void StretchBltAlphaModulate(int32_t destinationLeft, int32_t destinationTop, int32_t destinationWidth, int32_t destinationHeight, int32_t sourceWidth, int32_t sourceHeight, uint32_t gdTextureFormat, uint32_t gdType, void const* buffer, bool isUnknownFlagSet, void const* buffer2, uint32_t alpha) override;

		virtual void SetViewport(void) override;
		virtual void SetViewport(int32_t x, int32_t y, int32_t width, int32_t height) override;
		virtual void GetViewport(int32_t dimensions[4]) override;

		virtual char const* GetDriverInfo(void) const override;
		virtual uint32_t    GetGZCLSID(void) const override;

		// Init and Shutdown carry the same signature on cIGZGDriver and on
		// cIGZGBufferRegionExtension, so one definition satisfies both slots.
		virtual bool Init(void) override;
		virtual bool Shutdown(void) override;
		virtual bool IsDeviceReady(void) override;
		virtual bool Punt(uint32_t unknown, void* unknown2) override;

		// cIGZGBufferRegionExtension

		virtual bool     BufferRegionEnabled(void) override;
		virtual uint32_t NewBufferRegion(int32_t gdBufferRegionType) override;
		virtual bool     DeleteBufferRegion(int32_t bufferRegion) override;
		virtual bool     ReadBufferRegion(uint32_t region, int32_t x, int32_t y, int32_t width, int32_t height, int32_t screenX, int32_t screenY) override;
		virtual bool     DrawBufferRegion(uint32_t region, int32_t x, int32_t y, int32_t width, int32_t height, int32_t screenX, int32_t screenY) override;
		virtual bool     IsBufferRegion(uint32_t bufferRegion) override;
		virtual bool     CanDoPartialRegionWrites(void) override;
		virtual bool     CanDoOffsetReads(void) override;
		virtual bool     FinalRelease(void) override;
		virtual bool     DeleteAllBufferRegions(void) override;

		// cIGZGSnapshotExtension

		virtual cIGZBuffer* CopyColorBuffer(int32_t x, int32_t y, int32_t width, int32_t height, cIGZBuffer* buffer) override;

		// cIGZGDriverLightingExtension

		virtual void EnableLighting(bool isEnabled) override;
		virtual void EnableLight(uint32_t light, bool isEnabled) override;
		virtual void LightModelAmbient(float red, float green, float blue, float alpha) override;
		virtual void LightColor(uint32_t light, uint32_t type, float const* colour) override;
		virtual void LightColor(uint32_t light, float const* ambient, float const* diffuse, float const* specular) override;
		virtual void LightPosition(uint32_t light, float const* position) override;
		virtual void LightDirection(uint32_t light, float const* direction) override;
		virtual void MaterialColor(uint32_t type, float const* colour) override;
		virtual void MaterialColor(float const* ambient, float const* diffuse, float const* specular, float const* emission, float shininess) override;

		// cIGZGDriverVertexBufferExtension

		virtual char const* GetVertexBufferName(uint32_t gdVertexFormat) override;
		virtual uint32_t    VertexBufferType(uint32_t unknown) override;
		virtual uint32_t    MaxVertices(uint32_t unknown) override;
		virtual uint32_t    GetVertices(int32_t count, bool isUnknownFlagSet) override;
		virtual uint32_t    ContinueVertices(uint32_t unknown, uint32_t unknown2) override;
		virtual void        ReleaseVertices(uint32_t unknown) override;
		virtual void        DrawPrims(uint32_t unknown, uint32_t gdPrimitiveType, void* primitives, uint32_t count) override;
		virtual void        DrawPrimsIndexed(uint32_t unknown, uint32_t gdPrimitiveType, uint32_t count, uint16_t* indices, void* primitives, uint32_t secondCount) override;
		virtual void        Reset(void) override;
	};
}
