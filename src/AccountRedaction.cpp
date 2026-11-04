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

#include "AccountRedaction.h"

#include <windows.h>
#include <algorithm>
#include <string.h>

namespace scvk
{
	//// Constants

	namespace
	{
		constexpr char const* PROFILE_PLACEHOLDER = "%USERPROFILE%";
		constexpr char const* NAME_PLACEHOLDER    = "%USERNAME%";

		// What can come straight after a folder name in scvk's messages: a separator, or
		// something no file name holds or a message puts after a path. A space is among
		// them for a path at the end of a launch option.
		constexpr char const* FOLDER_END_CHARACTERS = "\\/\"' \t\r\n),;:";

		// The code pages the account is spelled in: Windows' ANSI one, then Vulkan's UTF-8.
		constexpr UINT CODE_PAGES[] = { CP_ACP, CP_UTF8 };
	}

	//// Private Functions

	namespace
	{
		bool IsSeparator(char character)
		{
			return character == '\\' || character == '/';
		}

		char ToLowerAscii(char character)
		{
			// Arithmetic on a char gives an int, and the result is still a lower case letter.
			return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
		}

		/** Whether the text spells the needle at a position, separators and ASCII letters in any case. */
		bool SpellsAt(std::string_view text, size_t position, std::string_view needle)
		{
			if (needle.empty() || text.size() - position < needle.size())
			{
				return false;
			}

			for (size_t i = 0; i < needle.size(); i++)
			{
				char const found  = text[position + i];
				char const wanted = needle[i];

				bool const areBothSeparators = IsSeparator(found) && IsSeparator(wanted);
				if (!areBothSeparators && ToLowerAscii(found) != ToLowerAscii(wanted))
				{
					return false;
				}
			}

			return true;
		}

		/** The first spelling that names a whole folder at a position, or null. */
		std::string const* FindFolderAt(std::string_view text, size_t position, std::vector<std::string> const& spellings)
		{
			for (std::string const& spelling : spellings)
			{
				size_t const end = position + spelling.size();
				bool const isWholeFolder = end == text.size() || (end < text.size() && strchr(FOLDER_END_CHARACTERS, text[end]) != nullptr);

				if (SpellsAt(text, position, spelling) && isWholeFolder)
				{
					return &spelling;
				}
			}

			return nullptr;
		}

		/** Wide text in a code page, or empty when it cannot be converted. */
		std::string ToCodePage(wchar_t const* text, UINT codePage)
		{
			int const size = WideCharToMultiByte(codePage, 0, text, -1, nullptr, 0, nullptr, nullptr);
			if (size <= 1)
			{
				return {};
			}

			// The size counts the terminator, which the string holds on its own, and is
			// positive here.
			std::string converted(static_cast<size_t>(size - 1), '\0');
			WideCharToMultiByte(codePage, 0, text, -1, converted.data(), size, nullptr, nullptr);

			return converted;
		}

		/** Adds an environment variable's spellings to a list, without trailing separators or repeats. */
		void AddSpellings(wchar_t const* variable, std::vector<std::string>& spellings)
		{
			wchar_t value[MAX_PATH] = {};
			DWORD const length = GetEnvironmentVariableW(variable, value, MAX_PATH);
			if (length == 0 || length >= MAX_PATH)
			{
				return;
			}

			for (UINT const codePage : CODE_PAGES)
			{
				std::string spelling = ToCodePage(value, codePage);
				while (!spelling.empty() && IsSeparator(spelling.back()))
				{
					spelling.pop_back();
				}

				if (!spelling.empty() && std::find(spellings.begin(), spellings.end(), spelling) == spellings.end())
				{
					spellings.push_back(spelling);
				}
			}
		}

		AccountSpellings ReadCurrentAccount(void)
		{
			// Windows sets both when the account signs in, so nothing beyond kernel32 is
			// needed to read them
			AccountSpellings account;
			AddSpellings(L"USERPROFILE", account.profileFolders);
			AddSpellings(L"USERNAME", account.names);

			return account;
		}
	}

	//// Public API

	std::string RedactAccount(std::string_view text, AccountSpellings const& account)
	{
		std::string redacted;
		redacted.reserve(text.size());

		size_t position = 0;

		while (position < text.size())
		{
			// A profile folder wherever it starts, and the name only as a folder of its own
			bool const isFolderStart = position > 0 && IsSeparator(text[position - 1]);

			std::string const* const profile = FindFolderAt(text, position, account.profileFolders);
			std::string const* const name    = (profile == nullptr && isFolderStart) ? FindFolderAt(text, position, account.names) : nullptr;

			if (profile != nullptr)
			{
				redacted += PROFILE_PLACEHOLDER;
				position += profile->size();
			}
			else if (name != nullptr)
			{
				redacted += NAME_PLACEHOLDER;
				position += name->size();
			}
			else
			{
				redacted += text[position];
				position++;
			}
		}

		return redacted;
	}

	AccountSpellings const& GetCurrentAccount(void)
	{
		static AccountSpellings const account = ReadCurrentAccount();
		return account;
	}
}
