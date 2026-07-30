/*
	Localization.h
	Text for the host/client welcome page, in English or Russian.

	Russian is selected by the presence of the Russian string folder in the game
	data (Common\Strings\Rus). No config key, no console switch: if the player
	installed the Russian data, they get a Russian page.

	SCOPE: this covers the welcome page only. The chat deliberately stays
	English - its overlay renders through ID3DXFont::DrawTextA, so putting
	non-ASCII through it risks breaking text that currently works.

	!! THIS FILE IS UTF-8 *WITH BOM* !!
	MSVC needs the BOM to decode the Cyrillic literals below; saved as UTF-8
	without one it assumes the system codepage and the wide strings come out as
	mojibake. Keep the BOM if you edit this file. Everything Cyrillic lives here
	so no other translation unit has to care about its encoding.
*/

#pragma once

#include <string>
#include <system_error>
#include <vector>

#include "SkinSystem.h"   // SkinSync::fs

namespace Loc
{
	/// Every string the welcome page shows. One entry per text control - the
	/// text control does not wrap, so the body is pre-split into lines.
	struct HostPageText
	{
		const wchar_t* title;
		const wchar_t* info1;
		const wchar_t* info2;
		const wchar_t* info3;
		const wchar_t* info4;
		const wchar_t* roleHost;
		const wchar_t* roleClient;
		const wchar_t* done;
	};

	inline const HostPageText& english()
	{
		static const HostPageText text =
		{
			L"THE HOBBIT SYNCHRONY",
			L"Welcome to the Hobbit Synchrony - a multiplayer",
			L"mod for The Hobbit. Your friends are waiting",
			L"for you. Choose to host a game or join a friend,",
			L"and let the journey begin.",
			L"ROLE:  HOST        (click to change)",
			L"ROLE:  CLIENT      (click to change)",
			L"Done",
		};
		return text;
	}

	inline const HostPageText& russian()
	{
		// Lines are kept about as long as the English ones so they still fit the
		// panel, and avoid characters a game font may not carry (no em dash, no
		// letter "ё").
		static const HostPageText text =
		{
			L"ХОББИТ СИНХРОНИЯ",
			L"Добро пожаловать в Хоббит Синхронию -",
			L"мультиплеерный мод для Хоббита.",
			L"Друзья уже ждут вас. Создайте игру или",
			L"присоединитесь к другу.",
			L"РОЛЬ:  ХОСТ        (нажмите для смены)",
			L"РОЛЬ:  КЛИЕНТ      (нажмите для смены)",
			L"Готово",
		};
		return text;
	}

	/// Is the Russian string data installed? Checked once - the data cannot
	/// appear mid-session.
	inline bool russianDataPresent()
	{
		static const bool present = []() -> bool
		{
			namespace fs = SkinSync::fs;

			// Roots: the current folder and the folder the game exe lives in.
			std::vector<std::string> roots;
			roots.push_back(".");

			char exePath[MAX_PATH] = {};
			const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
			if (length > 0 && length < MAX_PATH)
				roots.push_back(fs::path(exePath).parent_path().string());

			// Both shapes the data tree is laid out in. Windows compares paths
			// case-insensitively, so only the depth differs between these.
			static const char* const relatives[] =
			{
				"Common/STRINGS/RUS",
				"COMMON/STRINGS/RUS",
				"common/STRINGS/RUS",
			};

			for (const std::string& root : roots)
			{
				for (const char* relative : relatives)
				{
					std::error_code error;
					if (fs::is_directory(fs::path(root) / relative, error))
						return true;
				}
			}

			return false;
		}();

		return present;
	}

	inline const HostPageText& hostPage()
	{
		return russianDataPresent() ? russian() : english();
	}
}
