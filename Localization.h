/*
	Localization.h
	Text for the host/client welcome page, in English or Russian.

	Russian is selected by the presence of the Russian string folder in the game
	data (Common\Strings\Rus). No config key, no console switch: if the player
	installed the Russian data, they get a Russian page.

	THE RUSSIAN TEXT IS TRANSLITERATED INTO LATIN LETTERS, ON PURPOSE.
	The engine renders UI text from a bitmap font page
	(Common\UI\FontExtended.xbmp) whose glyphs are REDRAWN per localization, so
	the same character codes mean different letters in different builds: real
	Cyrillic literals rendered as garbage on a Russian install. Latin letters
	draw correctly everywhere, so Russian players get Russian wording spelled in
	Latin. Do not "fix" these strings back to Cyrillic without a font page that
	is known to carry it.

	SCOPE: this covers the welcome page only. The chat deliberately stays
	English - its overlay renders through ID3DXFont::DrawTextA, so putting
	non-ASCII through it risks breaking text that currently works.

	This file is pure ASCII, so its encoding no longer matters to MSVC.
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
		// TRANSLITERATED, not Cyrillic - deliberately.
		//
		// The engine draws UI text from a 256x512 BITMAP font page
		// (Common\UI\FontExtended.xbmp), and a localized build ships a REDRAWN
		// page: the same character codes carry different glyphs. Cyrillic
		// literals came out as garbage on a real Russian install, while plain
		// Latin renders correctly there - so Russian players get Russian
		// WORDING written in Latin letters, which every font page can draw.
		//
		// Lines are kept about as long as the English ones so they still fit
		// the panel.
		static const HostPageText text =
		{
			L"HOBBIT SINHRONIYA",
			L"Dobro pozhalovat v Hobbit Sinhroniyu -",
			L"multipleernyy mod dlya Hobbita. Druzya",
			L"uzhe zhdut vas. Sozdayte igru ili",
			L"prisoedinites k drugu.",
			L"ROL:  HOST          (nazhmite dlya smeny)",
			L"ROL:  KLIENT        (nazhmite dlya smeny)",
			L"Gotovo",
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
