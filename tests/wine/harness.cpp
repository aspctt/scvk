// Loads scvk.dll the way SimCity 4 does and drives it through the driver interface, to
// check it under Wine on Linux (or on Windows) without the game.
//
//   harness.exe [frames] [-fullscreen] [-LogLevel:debug]
//
// Draws a grid of grey squares through every draw path (plain and indexed, triangles and
// quads, 16 and 32-bit indices, vertex ranges that do not start at zero), presents the
// frames, and with -LogLevel:debug reads back scvk's frame capture to check every square
// landed where it should with the colour it was given. Exits non-zero on any failure.

#include <windows.h>

#include <cIGZCOMDirector.h>
#include <cIGZGDriver.h>
#include <ext/cIGZGDriverLightingExtension.h>
#include <sGDMode.h>
#include <VertexFormatUtils.h>

#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
	constexpr uint32_t DRIVER_GZCLSID = 0xC4554841;

	constexpr uint32_t PRIMITIVE_TRIANGLES = 0;
	constexpr uint32_t PRIMITIVE_QUADS     = 6;
	constexpr uint32_t INDEX_UNSIGNED_SHORT = 3;
	constexpr uint32_t INDEX_UNSIGNED_INT   = 5;
	constexpr uint32_t CLEAR_COLOUR_AND_DEPTH = 0x4000 | 0x1000;

	constexpr int COLUMNS = 32;
	constexpr int ROWS    = 24;
	constexpr int CELL    = 20;
	constexpr int SQUARE  = 16;

	struct Vertex
	{
		float   x, y, z;
		uint8_t colour[4];
	};

	// The upper half of the grid is drawn through the lighting extension: a directional
	// light along the normals, with this diffuse colour, over a quarter of ambient light,
	// both lighting the vertex colour.
	constexpr int   LIT_FIRST_ROW   = ROWS / 2;
	constexpr float LIT_AMBIENT     = 0.25f;
	constexpr float LIT_DIFFUSE[3]  = { 1.0f, 0.5f, 0.25f };

	struct LitVertex
	{
		float   x, y, z;
		float   nx, ny, nz;
		uint8_t colour[4];
	};

	uint8_t GreyOf(int column, int row);

	// The colour Direct3D 7 lights a square with, red, green, blue
	void ExpectedColour(int column, int row, bool isLightingTested, uint8_t out[3])
	{
		uint8_t const grey = GreyOf(column, row);
		for (int channel = 0; channel < 3; channel++)
		{
			float const value = (isLightingTested && row >= LIT_FIRST_ROW) ? (LIT_AMBIENT + LIT_DIFFUSE[channel]) * grey / 255.0f : grey / 255.0f;
			out[channel] = static_cast<uint8_t>(std::fmin(value, 1.0f) * 255.0f + 0.5f);
		}
	}

	uint8_t GreyOf(int column, int row)
	{
		return static_cast<uint8_t>((column * 7 + row * 13) % 200 + 40);
	}

	Vertex MakeVertex(float x, float y, uint8_t grey)
	{
		return Vertex{ x, y, 0.5f, { grey, grey, grey, 255 } };
	}

	int failures = 0;

	void Fail(char const* what)
	{
		std::printf("FAIL: %s\n", what);
		failures++;
	}

	// Reads a 32 bit BMP as scvk writes it
	bool ReadBmp(char const* path, int& width, int& height, std::vector<uint8_t>& pixels, bool& isTopDown)
	{
		FILE* file = std::fopen(path, "rb");
		if (file == nullptr)
		{
			return false;
		}

		BITMAPFILEHEADER fileHeader{};
		BITMAPINFOHEADER infoHeader{};
		bool const isRead = std::fread(&fileHeader, sizeof(fileHeader), 1, file) == 1 && std::fread(&infoHeader, sizeof(infoHeader), 1, file) == 1;

		if (!isRead || infoHeader.biBitCount != 32)
		{
			std::fclose(file);
			return false;
		}

		width     = infoHeader.biWidth;
		height    = infoHeader.biHeight < 0 ? -infoHeader.biHeight : infoHeader.biHeight;
		isTopDown = infoHeader.biHeight < 0;
		pixels.resize(size_t(width) * height * 4);
		std::fseek(file, static_cast<long>(fileHeader.bfOffBits), SEEK_SET);
		bool const hasPixels = std::fread(pixels.data(), 1, pixels.size(), file) == pixels.size();
		std::fclose(file);
		return hasPixels;
	}
}

int main(int argc, char** argv)
{
	int  frames          = 120;
	bool isFullscreen    = false;
	bool shouldCheckClip = false;
	bool isLightingTested = false;
	bool isCrashTested    = false;
	bool isTexEnvBlendTested = false;
	bool isPointLightTested  = false;

	for (int i = 1; i < argc; i++)
	{
		if (std::strcmp(argv[i], "-fullscreen") == 0) isFullscreen = true;
		else if (std::strcmp(argv[i], "-checkclipboard") == 0) shouldCheckClip = true;
		else if (std::strcmp(argv[i], "-lighting") == 0) isLightingTested = true;
		else if (std::strcmp(argv[i], "-crash") == 0) isCrashTested = true;
		else if (std::strcmp(argv[i], "-texenvblend") == 0) isTexEnvBlendTested = true;
		else if (std::strcmp(argv[i], "-pointlight") == 0) isPointLightTested = isLightingTested = true;
		else if (argv[i][0] != '-') frames = std::atoi(argv[i]);
	}

	// Load the plugin and make its driver, as the game's GZCOM does
	HMODULE const library = LoadLibraryA("scvk.dll");
	if (library == nullptr)
	{
		std::printf("FAIL: LoadLibrary(scvk.dll), error %lu\n", GetLastError());
		return 1;
	}

	using GetDirector = cIGZCOMDirector* (*)(void);
	GetDirector const getDirector = reinterpret_cast<GetDirector>(reinterpret_cast<void*>(GetProcAddress(library, "GZDllGetGZCOMDirector")));
	if (getDirector == nullptr || GetProcAddress(library, "SCVKRegisterFrameCallback") == nullptr || GetProcAddress(library, "SCVKUnregisterFrameCallback") == nullptr)
	{
		std::printf("FAIL: an export is missing\n");
		return 1;
	}

	cIGZCOMDirector* const director = getDirector();
	cIGZGDriver* driver = nullptr;
	if (director == nullptr || !director->GetClassObject(DRIVER_GZCLSID, GZIID_cIGZGDriver, reinterpret_cast<void**>(&driver)) || driver == nullptr)
	{
		std::printf("FAIL: no driver from the director\n");
		return 1;
	}

	if (!driver->Init())
	{
		std::printf("FAIL: Init, error %u\n", driver->GetError());
		return 1;
	}

	std::printf("driver: %s\n", driver->GetDriverInfo());

	// Pick a 640x480 mode of the kind asked for, or the first of that kind
	int32_t modeIndex = -1;
	sGDMode chosen{};
	uint32_t const modeCount = driver->CountVideoModes();

	for (uint32_t i = 0; i < modeCount; i++)
	{
		sGDMode mode{};
		driver->GetVideoModeInfo(i, mode);

		if (mode.isFullscreen != isFullscreen)
		{
			continue;
		}

		if (modeIndex < 0 || (mode.width == 640 && mode.height == 480))
		{
			modeIndex = static_cast<int32_t>(i);
			chosen    = mode;
		}
	}

	if (modeIndex < 0)
	{
		std::printf("FAIL: no %s mode among %u\n", isFullscreen ? "fullscreen" : "windowed", modeCount);
		return 1;
	}

	std::printf("mode %d: %ux%u %s\n", modeIndex, chosen.width, chosen.height, chosen.isFullscreen ? "fullscreen" : "windowed");
	driver->SetVideoMode(modeIndex, reinterpret_cast<void*>(&DefWindowProcA), false, false);

	uint32_t const error = driver->GetError();
	if (error != 0 || !driver->IsDeviceReady())
	{
		std::printf("FAIL: SetVideoMode, error %u, ready %d\n", error, driver->IsDeviceReady() ? 1 : 0);
		return 1;
	}

	// Fault on purpose, for the fault report scvk writes to its log
	if (isCrashTested)
	{
		std::printf("faulting on purpose\n");
		std::fflush(stdout);
		*static_cast<int volatile*>(nullptr) = 1;
	}

	int const width  = static_cast<int>(chosen.width);
	int const height = static_cast<int>(chosen.height);

	// Build the geometry: four vertices a square in one array, six in another, and the
	// indices into the first
	std::vector<Vertex>   quadVertices;
	std::vector<Vertex>   triangleVertices;
	std::vector<uint16_t> shortIndices;
	std::vector<uint32_t> intIndices;

	for (int row = 0; row < ROWS; row++)
	{
		for (int column = 0; column < COLUMNS; column++)
		{
			uint8_t const grey = GreyOf(column, row);
			float const left   = float(column * CELL + (CELL - SQUARE) / 2);
			float const bottom = float(row * CELL + (CELL - SQUARE) / 2);
			float const right  = left + SQUARE;
			float const top    = bottom + SQUARE;

			Vertex const a = MakeVertex(left, bottom, grey), b = MakeVertex(right, bottom, grey), c = MakeVertex(right, top, grey), d = MakeVertex(left, top, grey);
			quadVertices.insert(quadVertices.end(), { a, b, c, d });
			triangleVertices.insert(triangleVertices.end(), { a, b, c, a, c, d });
		}
	}

	std::vector<LitVertex> litVertices;
	for (Vertex const& vertex : quadVertices)
	{
		litVertices.push_back(LitVertex{ vertex.x, vertex.y, vertex.z, 0.0f, 0.0f, 1.0f, { vertex.colour[0], vertex.colour[1], vertex.colour[2], vertex.colour[3] } });
	}

	cIGZGDriverLightingExtension* lighting = nullptr;
	if (isLightingTested && (!driver->QueryInterface(GZIID_cIGZGDriverLightingExtension, reinterpret_cast<void**>(&lighting)) || lighting == nullptr))
	{
		std::printf("FAIL: no lighting extension\n");
		return 1;
	}

	for (int cell = 0; cell < COLUMNS * ROWS; cell++)
	{
		uint32_t const base = uint32_t(cell * 4);
		shortIndices.insert(shortIndices.end(), { uint16_t(base), uint16_t(base + 1), uint16_t(base + 2), uint16_t(base), uint16_t(base + 2), uint16_t(base + 3) });
		intIndices.insert(intIndices.end(), { base, base + 1, base + 2, base + 3 });
	}

	// An orthographic projection over the window, y up, as OpenGL has it
	float const projection[16] = {
		2.0f / float(width), 0, 0, 0,
		0, 2.0f / float(height), 0, 0,
		0, 0, -1, 0,
		-1, -1, 0, 1,
	};

	uint32_t const format    = driver->MakeVertexFormat(kGDVertexFormat_V3F_C4UB);
	uint32_t const litFormat = driver->MakeVertexFormat(kGDVertexFormat_V3F_N3F_C4UB);

	// Draw the frames
	// The texture environment's blend mode: one texel of known colour and alpha, over a
	// blue vertex colour, with a white environment colour
	struct TexturedVertex
	{
		float   x, y, z;
		uint8_t colour[4];
		float   u, v;
	};

	uint32_t blendTexture = 0;
	std::vector<TexturedVertex> blendQuad;
	uint32_t const texturedFormat = driver->MakeVertexFormat(kGDVertexFormat_V3F_C4UB_T2F);

	if (isTexEnvBlendTested)
	{
		uint8_t const texel[4] = { 255, 128, 0, 128 };   // RGBA
		driver->GenTextures(1, &blendTexture);
		driver->BindTexture(0, blendTexture);
		driver->TexImage2D(0, 0, 1, 1, 1, 0, 1, 1, texel);

		float const w = float(width) / 2.0f, h = float(height);
		uint8_t const blue[4] = { 255, 0, 0, 255 };     // BGRA
		blendQuad = {
			{ 0, 0, 0.5f, { blue[0], blue[1], blue[2], blue[3] }, 0.5f, 0.5f },
			{ w, 0, 0.5f, { blue[0], blue[1], blue[2], blue[3] }, 0.5f, 0.5f },
			{ w, h, 0.5f, { blue[0], blue[1], blue[2], blue[3] }, 0.5f, 0.5f },
			{ 0, h, 0.5f, { blue[0], blue[1], blue[2], blue[3] }, 0.5f, 0.5f },
		};
	}

	// A point light over the middle of one quad the size of the window, which faces it:
	// lit per pixel, the middle is brightest and the light falls off towards the edges;
	// lit per vertex, the four corners are equally far and the whole quad is one colour
	struct LitVertex
	{
		float   x, y, z;
		float   nx, ny, nz;
		uint8_t colour[4];
	};

	float const POINT_LIGHT_HEIGHT = 100.0f;
	std::vector<LitVertex> pointLightQuad;

	if (isPointLightTested)
	{
		float const w = float(width), h = float(height);
		pointLightQuad = {
			{ 0, 0, 0.5f, 0, 0, 1, { 255, 255, 255, 255 } },
			{ w, 0, 0.5f, 0, 0, 1, { 255, 255, 255, 255 } },
			{ w, h, 0.5f, 0, 0, 1, { 255, 255, 255, 255 } },
			{ 0, h, 0.5f, 0, 0, 1, { 255, 255, 255, 255 } },
		};
	}

	// Start with an empty clipboard, and keep drawing until PrintScreen fills it
	if (shouldCheckClip && OpenClipboard(nullptr))
	{
		EmptyClipboard();
		CloseClipboard();
	}

	ULONGLONG const clipboardDeadline = GetTickCount64() + 20000;
	int frame = 0;

	for (; frame < frames || (shouldCheckClip && !IsClipboardFormatAvailable(CF_DIB) && GetTickCount64() < clipboardDeadline); frame++)
	{
		MSG message;
		while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&message);
			DispatchMessageA(&message);
		}

		driver->SetViewport();
		driver->MatrixMode(1);
		driver->LoadMatrix(projection);
		driver->MatrixMode(0);
		driver->LoadIdentity();
		driver->Disable(kGDCapability_DepthTest);
		driver->Disable(kGDCapability_Texture2D);
		driver->Disable(kGDCapability_Blend);
		driver->Disable(kGDCapability_CullFace);
		driver->ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		driver->Clear(CLEAR_COLOUR_AND_DEPTH);

		// The game's own lighting state: the vertex colour lit by a white ambient light
		driver->EnableVertexColors(true, true);
		driver->ColorMultiplier(1.0f, 1.0f, 1.0f);
		driver->AlphaMultiplier(1.0f);

		if (isTexEnvBlendTested)
		{
			float const white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			driver->BindTexture(0, blendTexture);
			driver->Enable(kGDCapability_Texture2D);
			driver->TexEnv(0, kGDTextureEnvParamType_Color, white);
			driver->TexEnv(0, kGDTextureEnvParamType_Mode, kGDTextureEnvParam_Blend);
			driver->Enable(kGDCapability_Blend);
			driver->BlendFunc(4, 5);
			driver->InterleavedArrays(texturedFormat, 0, blendQuad.data());
			driver->DrawArrays(PRIMITIVE_QUADS, 0, 4);
			driver->Disable(kGDCapability_Blend);
			driver->Disable(kGDCapability_Texture2D);
			driver->TexEnv(0, kGDTextureEnvParamType_Mode, kGDTextureEnvParam_Modulate);
			driver->Flush();
			continue;
		}

		if (isPointLightTested && lighting != nullptr)
		{
			float const black[4]    = { 0.0f, 0.0f, 0.0f, 1.0f };
			float const white[4]    = { 1.0f, 1.0f, 1.0f, 1.0f };
			float const position[4] = { float(width) / 2.0f, float(height) / 2.0f, 0.5f + POINT_LIGHT_HEIGHT, 1.0f };
			driver->ColorMultiplier(0.0f, 0.0f, 0.0f);
			lighting->LightColor(0, black, white, black);
			lighting->LightPosition(0, position);
			lighting->EnableLight(0, true);
			driver->InterleavedArrays(litFormat, 0, pointLightQuad.data());
			driver->DrawArrays(PRIMITIVE_QUADS, 0, 4);
			lighting->EnableLight(0, false);
			driver->Flush();
			continue;
		}

		for (int cell = 0; cell < COLUMNS * ROWS; cell++)
		{
			// The lit half, through the lighting extension, with normals
			if (lighting != nullptr && cell / COLUMNS >= LIT_FIRST_ROW)
			{
				if (cell / COLUMNS == LIT_FIRST_ROW && cell % COLUMNS == 0)
				{
					float const black[4]   = { 0.0f, 0.0f, 0.0f, 1.0f };
					float const diffuse[4] = { LIT_DIFFUSE[0], LIT_DIFFUSE[1], LIT_DIFFUSE[2], 1.0f };
					float const along[3]   = { 0.0f, 0.0f, 1.0f };
					driver->ColorMultiplier(LIT_AMBIENT, LIT_AMBIENT, LIT_AMBIENT);
					lighting->LightColor(0, black, diffuse, black);
					lighting->LightDirection(0, along);
					lighting->EnableLight(0, true);
				}

				driver->InterleavedArrays(litFormat, 0, litVertices.data());
				if (cell % 2 == 0) driver->DrawArrays(PRIMITIVE_QUADS, cell * 4, 4);
				else driver->DrawElements(PRIMITIVE_QUADS, 4, INDEX_UNSIGNED_INT, intIndices.data() + cell * 4);
				continue;
			}

			switch (cell % 4)
			{
			case 0:
				driver->InterleavedArrays(format, 0, quadVertices.data());
				driver->DrawArrays(PRIMITIVE_QUADS, cell * 4, 4);
				break;

			case 1:
				driver->InterleavedArrays(format, 0, triangleVertices.data());
				driver->DrawArrays(PRIMITIVE_TRIANGLES, cell * 6, 6);
				break;

			case 2:
				driver->InterleavedArrays(format, 0, quadVertices.data());
				driver->DrawElements(PRIMITIVE_TRIANGLES, 6, INDEX_UNSIGNED_SHORT, shortIndices.data() + cell * 6);
				break;

			default:
				driver->InterleavedArrays(format, 0, quadVertices.data());
				driver->DrawElements(PRIMITIVE_QUADS, 4, INDEX_UNSIGNED_INT, intIndices.data() + cell * 4);
				break;
			}
		}

		if (lighting != nullptr)
		{
			lighting->EnableLight(0, false);
		}

		driver->Flush();

		if (driver->GetError() != 0)
		{
			Fail("an error after a frame");
			break;
		}
	}

	// Check what PrintScreen put on the clipboard, when a key press was sent from outside
	if (shouldCheckClip)
	{
		bool isChecked = false;

		if (OpenClipboard(nullptr))
		{
			HANDLE const data = GetClipboardData(CF_DIB);
			BITMAPINFOHEADER const* const header = data != nullptr ? static_cast<BITMAPINFOHEADER const*>(GlobalLock(data)) : nullptr;

			if (header != nullptr)
			{
				// Bottom-up rows; the square at column 0, row 0 sits at the bottom left
				uint8_t const* const bits = reinterpret_cast<uint8_t const*>(header + 1);
				int const x = CELL / 2, yUp = CELL / 2;
				uint8_t const* const pixel = bits + (size_t(yUp) * header->biWidth + x) * 4;
				std::printf("clipboard: %ldx%ld %u bpp, square 0,0 is %u,%u,%u (want %u)\n", header->biWidth, header->biHeight, header->biBitCount, pixel[0], pixel[1], pixel[2], GreyOf(0, 0));
				isChecked = header->biWidth == width && header->biHeight == height && header->biBitCount == 32 && pixel[0] == GreyOf(0, 0);
				GlobalUnlock(data);
			}

			CloseClipboard();
		}

		if (!isChecked)
		{
			Fail("the frame on the clipboard");
		}
	}

	// Check the picture scvk captured, when the log level asked it to capture one
	char path[MAX_PATH];
	GetModuleFileNameA(library, path, MAX_PATH);
	char* const slash = std::strrchr(path, '\\');
	std::snprintf(slash + 1, MAX_PATH - size_t(slash + 1 - path), "scvk-frame-2000.bmp");

	int captureWidth = 0, captureHeight = 0;
	bool isTopDown = false;
	std::vector<uint8_t> pixels;

	if (frames >= 2000)
	{
		if (!ReadBmp(path, captureWidth, captureHeight, pixels, isTopDown))
		{
			Fail("no frame capture to check");
		}
		else if (isTexEnvBlendTested)
		{
			// Blend: rgb = mix(blue, white, texel rgb) = (1, 0.502, 1), alpha 0.502,
			// blended over black: about (128, 64, 128)
			uint8_t const* const pixel = &pixels[(size_t(captureHeight / 2) * captureWidth + captureWidth / 4) * 4];
			std::printf("texture environment blend: rgb %u,%u,%u (want about 128,64,128)\n", pixel[2], pixel[1], pixel[0]);
			if (std::abs(pixel[2] - 128) > 3 || std::abs(pixel[1] - 64) > 3 || std::abs(pixel[0] - 128) > 3)
			{
				Fail("the texture environment's blend mode");
			}
		}
		else if (isPointLightTested)
		{
			// N.L = height / distance to the light, along the middle row
			int wrong = 0;
			for (int step = 0; step <= 4; step++)
			{
				int const x  = (width - 1) * step / 8;           // from the left edge to the middle
				int const yUp = height / 2;
				int const y  = isTopDown ? (captureHeight - 1 - yUp) : yUp;
				float const dx = float(x) + 0.5f - float(width) / 2.0f;
				float const want = 255.0f * POINT_LIGHT_HEIGHT / std::sqrt(dx * dx + POINT_LIGHT_HEIGHT * POINT_LIGHT_HEIGHT);
				uint8_t const* const pixel = &pixels[(size_t(y) * captureWidth + x) * 4];
				std::printf("point light: x %4d rgb %3u,%3u,%3u (want about %.0f)\n", x, pixel[2], pixel[1], pixel[0], want);
				if (std::fabs(float(pixel[2]) - want) > 4.0f || pixel[2] != pixel[1] || pixel[1] != pixel[0])
				{
					wrong++;
				}
			}

			if (wrong != 0)
			{
				Fail("the point light's fall-off across one quad");
			}
		}
		else
		{
			int wrong = 0;

			for (int row = 0; row < ROWS && row * CELL < captureHeight; row++)
			{
				for (int column = 0; column < COLUMNS && column * CELL < captureWidth; column++)
				{
					// The square's centre and a point between squares, in the capture's rows
					int const x       = column * CELL + CELL / 2;
					int const yUp     = row * CELL + CELL / 2;
					int const gapUp   = row * CELL;
					int const y       = isTopDown ? (captureHeight - 1 - yUp) : yUp;
					int const gapY    = isTopDown ? (captureHeight - 1 - gapUp) : gapUp;

					uint8_t const* const centre = &pixels[(size_t(y) * captureWidth + x) * 4];
					uint8_t const* const gap    = &pixels[(size_t(gapY) * captureWidth + column * CELL) * 4];
					uint8_t expected[3];
					ExpectedColour(column, row, isLightingTested, expected);

					// The capture is BGRA
					bool const isCentreRight = std::abs(centre[2] - expected[0]) <= 2 && std::abs(centre[1] - expected[1]) <= 2 && std::abs(centre[0] - expected[2]) <= 2;
					bool const isGapClear    = gap[0] == 0 && gap[1] == 0 && gap[2] == 0;

					if (!isCentreRight || !isGapClear)
					{
						if (wrong < 5)
						{
							std::printf("  cell %d,%d (path %d): centre rgb %u,%u,%u want %u,%u,%u; gap %u,%u,%u\n", column, row, (row * COLUMNS + column) % 4, centre[2], centre[1], centre[0], expected[0], expected[1], expected[2], gap[0], gap[1], gap[2]);
						}

						wrong++;
					}
				}
			}

			std::printf("capture %dx%d: %d of %d squares wrong\n", captureWidth, captureHeight, wrong, COLUMNS * ROWS);
			if (wrong != 0)
			{
				Fail("squares in the capture");
			}
		}
	}

	// Hide the mode and shut down, as the game does on exit
	driver->SetVideoMode(-1, nullptr, false, false);
	driver->Shutdown();
	driver->Release();

	std::printf(failures == 0 ? "PASS (%d frames)\n" : "FAILED (%d frames)\n", frame);
	return failures == 0 ? 0 : 1;
}
