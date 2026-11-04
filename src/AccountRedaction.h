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

#include <string>
#include <string_view>
#include <vector>

namespace scvk
{
	//// Types

	/**
	 * How text can spell the Windows account, without trailing separators. Paths reach
	 * the log through Windows' ANSI functions, in the code page, while Vulkan hands its
	 * strings over in UTF-8, so each list holds both spellings where they differ.
	 */
	struct AccountSpellings
	{
		std::vector<std::string> profileFolders;  // such as C:\Users\Jane Doe
		std::vector<std::string> names;           // such as Jane Doe
	};

	//// Public API

	/**
	 * The text with the account taken out, as SCD3D11 does with its own log. Players post
	 * their logs and screenshots of the setup dialog in public, and the paths in them
	 * name the Windows account, which is often a real name. A profile folder becomes
	 * %USERPROFILE%, and the account name as a folder of its own, such as
	 * D:\Jane Doe\Games, becomes %USERNAME%. Separators and ASCII letters match in any
	 * case, and a longer folder name such as Jane Does stays.
	 */
	std::string RedactAccount(std::string_view text, AccountSpellings const& account);

	/** The account the game runs under, read from the environment on the first call. */
	AccountSpellings const& GetCurrentAccount(void);
}
