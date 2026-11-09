// Head Body Color Match: an F4SE plugin that matches the body skin colour of Fallout 4 NPCs to their face.
// Copyright (C) 2026 mrtony - https://www.nexusmods.com/fallout4/mods/110802
// Fallout 4 VR 1.2.72 port (CommonLibF4VR, F4SE VR): 2026 Axel, with the VR addresses checked against an unpacked
// Fallout4VR.exe 1.2.72. Changes from 1.1.0 are marked "VR:".
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
// Public License as published by the Free Software Foundation, version 3 of the License, with the modding
// exception in EXCEPTIONS. This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details. You should have received a copy of the GNU General Public License
// along with this program (LICENSE); if not, see <https://www.gnu.org/licenses/>.

#include "pch.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <map>
#include <set>
#include <vector>

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>
#undef ERROR

namespace
{
	// Not in CommonLibF4; layout from ianpatt/f4se NiMaterials.h.
	// VR: BSLightingShaderMaterialBase is 0x10 smaller (0xB0), so tintColor sits at 0xB0: Fallout4VR.exe 1.2.72
	// BSLightingShaderMaterialSkinTint::Create (0x280F6B0) allocates 0xC0 and zeroes 0xB0..0xBF, CopyMembers
	// (0x280F730) copies 0xB0..0xBF. A flat 0xC0 write would land past the end of the material.
	struct SkinTintMaterial : RE::BSLightingShaderMaterialBase
	{
		RE::NiColorA tintColor;  // B0 (VR)
	};
	static_assert(sizeof(RE::BSLightingShaderMaterialBase) == 0xB0);
	static_assert(offsetof(SkinTintMaterial, tintColor) == 0xB0);
	static_assert(sizeof(SkinTintMaterial) == 0xC0);

	struct ToneBand
	{
		float lmin;
		float lmax;
		float f[3];
	};

	struct Settings
	{
		bool                                         enabled{ true };
		float                                        strength{ 1.0f };
		int                                          reloadKey{ 0x79 };  // VK_F10
		int                                          refresh{ 1 };  // 0 = none, 1 = rebuild actor 3D after a reload
		bool                                         includePlayer{ false };
		std::map<RE::TESFormID, std::array<float, 3>> overrides;
		std::map<RE::TESFormID, std::array<float, 3>> rearOverrides;
		std::vector<ToneBand>                        bands;
		std::vector<ToneBand>                        bandsMale;
		std::vector<ToneBand>                        bandsPlayer;
		std::vector<ToneBand>                        bandsPlayerMale;
	};
	Settings           g_settings;
	std::mutex         g_settingsLock;  // taken after g_lock, never before
	std::atomic<int>   g_reloadKey{ 0x79 };
	constexpr auto     kIniPath = "Data/F4SE/Plugins/HeadBodyColorMatch.ini";

	float Luma(float r, float g, float b) { return 0.299f * r + 0.587f * g + 0.114f * b; }

	struct Law
	{
		bool          skip{ false };  // no NPC or disabled: leave the material alone
		const char*   rule{ "" };
		float         lum{ 0 };
		float         factor[3]{ 1, 1, 1 };
		RE::TESFormID baseId{ 0 };
		RE::TESFormID faceId{ 0 };
		bool          female{ false };
		bool          rear{ false };
	};

	constexpr auto WS = " \t\r\n";

	std::string Trim(const std::string& a_s)
	{
		const auto b = a_s.find_first_not_of(WS);
		if (b == std::string::npos) {
			return {};
		}
		return a_s.substr(b, a_s.find_last_not_of(WS) - b + 1);
	}

	std::vector<float> ParseFloats(const std::string& a_s)
	{
		std::vector<float> out;
		std::size_t        pos = 0;
		while (pos <= a_s.size()) {
			auto end = a_s.find(',', pos);
			if (end == std::string::npos) {
				end = a_s.size();
			}
			out.push_back(std::stof(Trim(a_s.substr(pos, end - pos))));
			pos = end + 1;
		}
		return out;
	}

	void LoadSettings()
	{
		Settings    s;
		std::ifstream f(kIniPath);
		std::string line;
		while (std::getline(f, line)) {
			line = Trim(line);
			const auto eq = line.find('=');
			if (eq == std::string::npos || line[0] == ';' || line[0] == '#') {
				continue;
			}
			const auto key = Trim(line.substr(0, eq));
			const auto val = Trim(line.substr(eq + 1));
			try {
				if (key == "bEnabled") {
					s.enabled = std::stoi(val) != 0;
				} else if (key == "fStrength") {
					s.strength = std::stof(val);
				} else if (key == "iReloadKey") {
					s.reloadKey = std::stoi(val);
				} else if (key == "bIncludePlayer") {
					s.includePlayer = std::stoi(val) != 0;
				} else if (key == "iRefresh") {
					s.refresh = std::clamp(std::stoi(val), 0, 1);
				} else if (key.rfind("OverrideRear.", 0) == 0) {
					const auto v = ParseFloats(val);
					if (v.size() != 3) {
						throw std::invalid_argument("override needs R,G,B");
					}
					s.rearOverrides[static_cast<RE::TESFormID>(std::stoul(key.substr(13), nullptr, 16))] = { v[0], v[1], v[2] };
				} else if (key.rfind("Override.", 0) == 0) {
					const auto v = ParseFloats(val);
					if (v.size() != 3) {
						throw std::invalid_argument("override needs R,G,B");
					}
					s.overrides[static_cast<RE::TESFormID>(std::stoul(key.substr(9), nullptr, 16))] = { v[0], v[1], v[2] };
				} else if (key == "ToneBand") {
					const auto v = ParseFloats(val);
					if (v.size() != 5) {
						throw std::invalid_argument("toneband needs Lmin,Lmax,R,G,B");
					}
					s.bands.push_back({ v[0], v[1], { v[2], v[3], v[4] } });
				} else if (key == "ToneBandMale") {
					const auto v = ParseFloats(val);
					if (v.size() != 5) {
						throw std::invalid_argument("tonebandmale needs Lmin,Lmax,R,G,B");
					}
					s.bandsMale.push_back({ v[0], v[1], { v[2], v[3], v[4] } });
				} else if (key == "ToneBandPlayer" || key == "ToneBandPlayerMale") {
					const auto v = ParseFloats(val);
					if (v.size() != 5) {
						throw std::invalid_argument("tonebandplayer needs Lmin,Lmax,R,G,B");
					}
					(key == "ToneBandPlayer" ? s.bandsPlayer : s.bandsPlayerMale).push_back({ v[0], v[1], { v[2], v[3], v[4] } });
				}
			} catch (...) {
				F4SE::log::warn("settings: ignored bad line '{}'", line);
			}
		}
		F4SE::log::info("settings: bEnabled={} fStrength={} iReloadKey={} overrides={} rearOverrides={} tonebands={} toneBandsMale={} toneBandsPlayer={} toneBandsPlayerMale={} iRefresh={} bIncludePlayer={}", s.enabled, s.strength, s.reloadKey, s.overrides.size(), s.rearOverrides.size(), s.bands.size(), s.bandsMale.size(), s.bandsPlayer.size(), s.bandsPlayerMale.size(), s.refresh, s.includePlayer);
		for (auto& [id, v] : s.overrides) {
			F4SE::log::info("settings:   Override.{:08X} = ({:.3f},{:.3f},{:.3f})", id, v[0], v[1], v[2]);
		}
		for (auto& [id, v] : s.rearOverrides) {
			F4SE::log::info("settings:   OverrideRear.{:08X} = ({:.3f},{:.3f},{:.3f})", id, v[0], v[1], v[2]);
		}
		for (auto& b : s.bands) {
			F4SE::log::info("settings:   ToneBand L[{:.3f},{:.3f}] = ({:.3f},{:.3f},{:.3f})", b.lmin, b.lmax, b.f[0], b.f[1], b.f[2]);
		}
		for (auto& b : s.bandsMale) {
			F4SE::log::info("settings:   ToneBandMale L[{:.3f},{:.3f}] = ({:.3f},{:.3f},{:.3f})", b.lmin, b.lmax, b.f[0], b.f[1], b.f[2]);
		}
		for (auto& b : s.bandsPlayer) {
			F4SE::log::info("settings:   ToneBandPlayer L[{:.3f},{:.3f}] = ({:.3f},{:.3f},{:.3f})", b.lmin, b.lmax, b.f[0], b.f[1], b.f[2]);
		}
		for (auto& b : s.bandsPlayerMale) {
			F4SE::log::info("settings:   ToneBandPlayerMale L[{:.3f},{:.3f}] = ({:.3f},{:.3f},{:.3f})", b.lmin, b.lmax, b.f[0], b.f[1], b.f[2]);
		}
		g_reloadKey = s.reloadKey;
		std::lock_guard lock(g_settingsLock);
		g_settings = std::move(s);
	}

	// ianpatt/f4se GameFormComponents.h: BipedAnim::AttachSkinnedObject, RVA 0x35E960 in 1.11.240 and 0x1C3A10 in 1.10.163; it opens with mov [rsp+20h], r9d.
	// VR: 0x1CBC00 in 1.2.72 (VR Address Library), same prologue and the same 5 arguments (the 5th, a bool, is read at [rbp+B0]).
	constexpr std::uint64_t               kAttachSkinnedObjectID = 1575810;
	constexpr std::array<std::uint8_t, 5> kAttachPrologue{ 0x44, 0x89, 0x4C, 0x24, 0x20 };
	constexpr auto                        kLoadedRewalkDelay = 1500ms;
	constexpr std::array                  kPostLoadDelays{ 2000ms, 6000ms };
	constexpr auto                        kPostLoadFieldDelay = 500ms;

	struct Written
	{
		RE::NiColorA original;  // colour before any write of ours
		RE::NiColorA written;   // colour we last stored
	};

	std::unordered_map<const void*, Written>        g_materials;
	std::unordered_map<RE::TESFormID, RE::NiColorA> g_actorOriginal;  // actor -> original of its first material
	std::mutex                                      g_lock;

	bool Near(const RE::NiColorA& a, const RE::NiColorA& b)
	{
		return std::abs(a.r - b.r) < 1e-4f && std::abs(a.g - b.g) < 1e-4f && std::abs(a.b - b.b) < 1e-4f;
	}

	constexpr RE::TESFormID kPlayerBaseId = 0x7;

	Law ComputeLaw(RE::TESNPC* npc, const RE::NiColorA& a_orig, bool a_rear)
	{
		Law             law;
		law.rear = a_rear;
		std::lock_guard slock(g_settingsLock);
		if (!g_settings.enabled) {
			law.skip = true;
			law.rule = "disabled";
			return law;
		}
		const RE::TESNPC* face = npc ? npc->GetRootFaceNPC() : nullptr;
		law.baseId = npc ? npc->GetFormID() : 0;
		law.faceId = face ? face->GetFormID() : 0;
		law.lum = Luma(a_orig.r, a_orig.g, a_orig.b);
		law.female = npc && npc->GetSex() == RE::SEX::kFemale;
		if (!npc) {
			law.skip = true;
			law.rule = "skip-no-npc";
			return law;
		}
		if (law.baseId == kPlayerBaseId && !g_settings.includePlayer) {
			law.skip = true;
			law.rule = "skip-player(bIncludePlayer=0)";
			return law;
		}
		float f[3]{ 1, 1, 1 };
		law.rule = "none";
		auto it = g_settings.overrides.find(law.faceId);
		if (it == g_settings.overrides.end()) {
			it = g_settings.overrides.find(law.baseId);
		}
		if (it != g_settings.overrides.end()) {
			std::copy(it->second.begin(), it->second.end(), f);
			law.rule = "override";
			if (a_rear) {
				auto rit = g_settings.rearOverrides.find(law.faceId);
				if (rit == g_settings.rearOverrides.end()) {
					rit = g_settings.rearOverrides.find(law.baseId);
				}
				if (rit != g_settings.rearOverrides.end()) {
					std::copy(rit->second.begin(), rit->second.end(), f);
					law.rule = "override-rear";
				} else {
					law.rule = "override(rear: no OverrideRear, body factor)";
				}
			}
		} else {
			// The player's head is built live, not baked: its own bands first, the NPC bands where none matches.
			const auto pick = [&](const std::vector<ToneBand>& a_bands, const char* a_rule) {
				for (const auto& band : a_bands) {
					if (law.lum >= band.lmin && law.lum <= band.lmax) {
						std::copy(std::begin(band.f), std::end(band.f), f);
						law.rule = a_rule;
						return true;
					}
				}
				return false;
			};
			if (law.baseId != kPlayerBaseId || !pick(law.female ? g_settings.bandsPlayer : g_settings.bandsPlayerMale, "toneband-player")) {
				pick(law.female ? g_settings.bands : g_settings.bandsMale, "toneband");
			}
		}
		for (int c = 0; c < 3; ++c) {
			law.factor[c] = 1.0f + (f[c] - 1.0f) * g_settings.strength;
		}
		return law;
	}

	RE::TESNPC* BaseNpcOf(RE::Actor* a_actor)
	{
		return a_actor->GetNPC();
	}

	Law ComputeLaw(RE::Actor* a_actor, const RE::NiColorA& a_orig, bool a_rear)
	{
		return ComputeLaw(BaseNpcOf(a_actor), a_orig, a_rear);
	}

	RE::NiColorA Tinted(const RE::NiColorA& a_orig, const Law& a_law)
	{
		RE::NiColorA c = a_orig;
		if (a_law.skip) {
			return c;
		}
		c.r = std::clamp(a_orig.r * a_law.factor[0], 0.0f, 2.0f);
		c.g = std::clamp(a_orig.g * a_law.factor[1], 0.0f, 2.0f);
		c.b = std::clamp(a_orig.b * a_law.factor[2], 0.0f, 2.0f);
		return c;
	}

	// TESNPC::skinColor (f4se GameObjects.h, UInt8 R,G,B,A at 0x2EA; CommonLibF4 bodyTintColorR..A, same offset) is the NPC's QNAM skin tone.
	// VR: also 0x2EA in 1.2.72 (the skin colour function 0x5A03A0 writes [npc+2EA..2ED]).
	using SkinBytes = std::array<std::uint8_t, 4>;
	std::map<RE::TESFormID, SkinBytes> g_npcOriginal;  // the value the game holds per NPC, as last seen when it was not ours
	std::map<RE::TESFormID, SkinBytes> g_npcWritten;   // the last value we wrote to the field
	std::mutex                         g_npcLock;

	SkinBytes ReadSkin(const RE::TESNPC* a_npc)
	{
		// VR: CommonLibF4VR declares the four bytes as std::int8_t; they are unsigned 0..255 colour channels.
		return { static_cast<std::uint8_t>(a_npc->bodyTintColorR), static_cast<std::uint8_t>(a_npc->bodyTintColorG), static_cast<std::uint8_t>(a_npc->bodyTintColorB),
			static_cast<std::uint8_t>(a_npc->bodyTintColorA) };
	}

	void WriteSkin(RE::TESNPC* a_npc, const SkinBytes& a_v)
	{
		a_npc->bodyTintColorR = static_cast<std::int8_t>(a_v[0]);
		a_npc->bodyTintColorG = static_cast<std::int8_t>(a_v[1]);
		a_npc->bodyTintColorB = static_cast<std::int8_t>(a_v[2]);
		a_npc->bodyTintColorA = static_cast<std::int8_t>(a_v[3]);
	}

	RE::NiColorA SkinColor(const SkinBytes& a_v)
	{
		return RE::NiColorA{ a_v[0] / 255.0f, a_v[1] / 255.0f, a_v[2] / 255.0f, a_v[3] / 255.0f };
	}

	// The original is the value the game holds: a field value that is not our last write came from the game (esm record, save load) and becomes the new original.
	// A save-created NPC (FormID 0xFF......) stores our written colour in the save, so its original is the one of its esm root face NPC.
	SkinBytes TrackFieldOriginal(const RE::TESNPC* a_npc)
	{
		const auto      id = a_npc->GetFormID();
		std::lock_guard lock(g_npcLock);
		const auto      cur = ReadSkin(a_npc);
		if (const auto w = g_npcWritten.find(id); w != g_npcWritten.end() && w->second == cur) {
			return g_npcOriginal.try_emplace(id, cur).first->second;
		}
		g_npcWritten.erase(id);
		const auto [it, fresh] = g_npcOriginal.try_emplace(id, cur);
		if (!fresh && it->second != cur) {
			it->second = cur;
		}
		return it->second;
	}

	SkinBytes TrackNpcOriginal(const RE::TESNPC* a_npc)
	{
		const auto id = a_npc->GetFormID();
		if (id >= 0xFF000000) {
			const auto* face = a_npc->GetRootFaceNPC();
			if (face && face != a_npc && face->GetFormID() < 0xFF000000) {
				return TrackFieldOriginal(face);
			}
		}
		return TrackFieldOriginal(a_npc);
	}

	// Main thread. Sets the NPC's skin field to original x factor (restores the original when the law skips it). Returns true when the field changed.
	bool ApplyNpc(RE::TESNPC* a_npc)
	{
		if (!a_npc) {
			return false;
		}
		const auto id = a_npc->GetFormID();
		const auto orig = TrackNpcOriginal(a_npc);
		const auto cur = ReadSkin(a_npc);
		const auto law = ComputeLaw(a_npc, SkinColor(orig), false);
		SkinBytes  target = orig;
		bool       clamped = false;
		if (!law.skip) {
			for (int c = 0; c < 3; ++c) {
				const float v = orig[c] / 255.0f * law.factor[c];
				clamped = clamped || v > 1.0f;
				target[c] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
			}
		}
		const bool changed = target != cur;
		if (changed) {
			std::lock_guard lock(g_npcLock);
			WriteSkin(a_npc, target);
			g_npcWritten[id] = target;
		}
		if (changed && clamped) {
			F4SE::log::debug("NPC {:08X}: factor ({:.3f},{:.3f},{:.3f}) clamped at 255, the skin field's ceiling", id, law.factor[0], law.factor[1], law.factor[2]);
		}
		return changed;
	}

	// Main thread. Every NPC form, plus the base NPCs of loaded actors (runtime-created ones are not in the form array).
	std::set<RE::TESFormID> ApplyAllNpcs()
	{
		std::set<RE::TESFormID> changed;
		int                     seen = 0;
		if (auto* dh = RE::TESDataHandler::GetSingleton()) {
			for (auto* npc : dh->GetFormArray<RE::TESNPC>()) {
				++seen;
				if (ApplyNpc(npc)) {
					changed.insert(npc->GetFormID());
				}
			}
		}
		const auto applyActor = [&](RE::Actor* a_actor) {
			if (auto* npc = a_actor ? BaseNpcOf(a_actor) : nullptr) {
				if (ApplyNpc(npc)) {
					changed.insert(npc->GetFormID());
				}
			}
		};
		if (auto player = RE::PlayerCharacter::GetPlayerHandle().get()) {
			applyActor(player.get());
		}
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			std::vector<RE::ActorHandle> handles(lists->highActorHandles.begin(), lists->highActorHandles.end());
			for (auto& h : handles) {
				if (auto actor = h.get()) {
					applyActor(actor.get());
				}
			}
		}
		std::lock_guard slock(g_settingsLock);
		for (auto& [id, v] : g_settings.rearOverrides) {
			if (const auto it = g_settings.overrides.find(id); it == g_settings.overrides.end() || it->second != v) {
				F4SE::log::warn("[npcField] OverrideRear.{:08X} differs from Override: the nape shares the NPC skin field with the body, so only the material fallback can differ (applied after the 3D is built, may not show)", id);
			}
		}
		F4SE::log::debug("[npcField] applied over {} NPC forms: {} fields changed", seen, changed.size());
		return changed;
	}

	void LogBytes(const char* a_what, std::uintptr_t a_addr)
	{
		const auto* b = reinterpret_cast<const std::uint8_t*>(a_addr);
		F4SE::log::error("{} bytes at {:X}: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}", a_what, a_addr, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
	}

	bool PrologueMatches(std::uintptr_t a_address, const std::array<std::uint8_t, 8>& a_bytes)
	{
		return std::memcmp(reinterpret_cast<const void*>(a_address), a_bytes.data(), a_bytes.size()) == 0;
	}

	// The first E8 inside a_function (within its .pdata bounds) whose target is a_callee; 0 when there is none.
	std::uintptr_t FindCall(std::uintptr_t a_function, std::uintptr_t a_callee)
	{
		DWORD64     imageBase = 0;
		const auto* entry = ::RtlLookupFunctionEntry(a_function, &imageBase, nullptr);
		if (!entry || imageBase + entry->BeginAddress != a_function) {
			return 0;
		}
		for (auto site = a_function; site + 5 <= imageBase + entry->EndAddress; ++site) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(site);
			if (bytes[0] == 0xE8 && site + 5 + *reinterpret_cast<const std::int32_t*>(bytes + 1) == a_callee) {
				return site;
			}
		}
		return 0;
	}

	DWORD                               g_mainThread = 0;
	constexpr int                       kMainThreadMaxAttempts = 100;
	constexpr std::chrono::milliseconds kMainThreadRetryDelay{ 100 };

	// Returns true when the geometry carries a skin-tint material.
	bool ProcessGeometry(RE::Actor* a_actor, RE::BSGeometry* a_geom)
	{
		bool       found = false;
		const auto id = a_actor->GetFormID();
		for (auto& prop : a_geom->GetRuntimeData().properties) {
			auto* shader = netimmerse_cast<RE::BSLightingShaderProperty*>(prop.get());
			if (!shader || !shader->material) {
				continue;
			}
			++g_dbgShaders;
			auto* mat = shader->material;
			if (mat->GetFeature() != RE::BSShaderMaterial::Feature::kSkinTint) {
				continue;
			}
			found = true;
			auto* skin = static_cast<SkinTintMaterial*>(mat);

			std::lock_guard lock(g_lock);
			const auto      shapeName = a_geom->name.c_str();
			const bool      isRear = shapeName && (std::strstr(shapeName, "HeadHumanRear") || std::strstr(shapeName, "HeadRear"));
			const auto      cur = skin->tintColor;

			auto*       npc = BaseNpcOf(a_actor);
			auto        it = g_materials.find(mat);
			const bool  known = it != g_materials.end() && Near(cur, it->second.written);
			const bool  haveField = npc != nullptr;
			const auto  origBytes = haveField ? TrackNpcOriginal(npc) : SkinBytes{};
			const auto  curBytes = haveField ? ReadSkin(npc) : SkinBytes{};
			RE::NiColorA orig = cur;
			const auto closeTo = [&](const SkinBytes& v) {
				const auto c = SkinColor(v);
				return std::abs(cur.r - c.r) <= 0.012f && std::abs(cur.g - c.g) <= 0.012f && std::abs(cur.b - c.b) <= 0.012f;
			};
			if (haveField && (known || closeTo(origBytes) || closeTo(curBytes))) {
				orig = SkinColor(origBytes);  // the NPC's untouched value: nothing stacks
				orig.a = cur.a;
			} else if (known) {
				orig = it->second.original;
			} else if (const auto prior = g_actorOriginal.find(id); prior != g_actorOriginal.end() && Near(cur, Tinted(prior->second, ComputeLaw(a_actor, prior->second, isRear)))) {
				orig = prior->second;
			}
			g_actorOriginal.try_emplace(id, orig);

			const auto law = ComputeLaw(a_actor, orig, isRear);
			const auto target = Tinted(orig, law);  // equals orig when skipped: restores a colour we changed earlier
			const bool identity = law.skip || (law.factor[0] == 1.0f && law.factor[1] == 1.0f && law.factor[2] == 1.0f);
			const bool near8 = std::abs(cur.r - target.r) <= 0.004f && std::abs(cur.g - target.g) <= 0.004f && std::abs(cur.b - target.b) <= 0.004f;
			if (identity && (near8 || Near(cur, target))) {
				g_materials.insert_or_assign(mat, Written{ orig, cur });
				continue;
			}
			skin->tintColor = target;
			g_materials.insert_or_assign(mat, Written{ orig, skin->tintColor });
		}
		return found;
	}

	// VR: CommonLibF4VR's Ni_RTTI::BSGeometry ID (1287097) resolves to 0x3884F18 in 1.2.72, which is not an NiRTTI, so
	// netimmerse_cast<BSGeometry*> never matched and no shape was ever tinted (port 3 log: "25 actors, 0 skin-tint shapes").
	// The real NiRTTI of BSGeometry is built at startup by the constructor call at 0x2B1D197 (name string in rdx, object in rcx):
	// 0x5C62A30. The same arithmetic gives 0x5C61800 for NiNode and 0x6878018 for BSLightingShaderProperty, which are the
	// addresses of their (correct) VR IDs. The name field is checked before the address is trusted.
	constexpr std::uintptr_t kBSGeometryRttiRva = 0x5C62A30;

	const RE::NiRTTI* BSGeometryRtti()
	{
		static const RE::NiRTTI* rtti = [] -> const RE::NiRTTI* {
			const auto* r = reinterpret_cast<const RE::NiRTTI*>(REL::Module::get().base() + kBSGeometryRttiRva);
			if (!r->name || std::strcmp(r->name, "BSGeometry") != 0) {
				F4SE::log::error("BSGeometry NiRTTI not found at RVA {:X}: no shape will be tinted", kBSGeometryRttiRva);
				return nullptr;
			}
			F4SE::log::info("BSGeometry NiRTTI at RVA {:X}", kBSGeometryRttiRva);
			return r;
		}();
		return rtti;
	}

	std::atomic<int> g_dbgGeoms{ 0 };    // geometries reached by the walk (debug counters, logged after a rewalk)
	std::atomic<int> g_dbgShaders{ 0 };  // of them, with a BSLightingShaderProperty that has a material

	void Walk(RE::Actor* a_actor, RE::NiAVObject* a_obj, int a_depth, int& a_shapes)
	{
		if (!a_obj || a_depth > 32) {
			return;
		}
		if (const auto* geomRtti = BSGeometryRtti(); geomRtti && a_obj->GetRTTI() && a_obj->GetRTTI()->IsKindOf(geomRtti)) {
			++g_dbgGeoms;
			if (ProcessGeometry(a_actor, static_cast<RE::BSGeometry*>(a_obj))) {
				++a_shapes;
			}
		}
		if (auto* node = netimmerse_cast<RE::NiNode*>(a_obj)) {
			for (auto& child : node->GetRuntimeData().children) {
				Walk(a_actor, child.get(), a_depth + 1, a_shapes);
			}
		}
	}

	// The looks menu rebuilds the edited actor on a loader thread at every step; a main-thread walk of that 3D meanwhile crashed the game.
	std::atomic<bool> g_looksMenuOpen{ false };

	// Walks a subtree; returns the number of skin-tint shapes found. a_inBuild: called from inside the game's own build of a_root (the hooks).
	int WalkRoot(RE::Actor* a_actor, RE::NiAVObject* a_root, bool a_inBuild = false)
	{
		if (!a_actor || !a_root || (g_looksMenuOpen && !a_inBuild)) {
			return 0;
		}
		int shapes = 0;
		Walk(a_actor, a_root, 0, shapes);
		return shapes;
	}

	// Main thread only. Third person 3D, plus first person for the player.
	int WalkActor(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return 0;
		}
		int shapes = 0;
		if (auto* root = a_actor->Get3D()) {
			shapes += WalkRoot(a_actor, root);
		}
		if (a_actor->GetFormID() == 0x14) {
			if (auto* root = a_actor->Get3D(true)) {
				shapes += WalkRoot(a_actor, root);
			}
		}
		return shapes;
	}

	// Due Timer work waiting for the main-loop hook; without the hook it goes to the F4SE task queue, which runs it on whichever thread drains it.
	std::atomic<bool>                  g_framePumpInstalled{ false };
	std::atomic<bool>                  g_frameJobsPending{ false };
	std::mutex                         g_frameLock;
	std::vector<std::function<void()>> g_frameJobs;

	// Runs a_work on the main thread at the end of the next frame; only valid once g_framePumpInstalled is true.
	void PostToFrame(std::function<void()> a_work)
	{
		std::lock_guard lock(g_frameLock);
		g_frameJobs.push_back(std::move(a_work));
		g_frameJobsPending = true;
	}

	// The timer thread only sleeps; the work goes back to the main thread through the main-loop hook.
	class Timer
	{
	public:
		static Timer& Get()
		{
			static Timer instance;
			return instance;
		}

		void Post(std::chrono::milliseconds a_delay, std::function<void()> a_work)
		{
			{
				std::lock_guard lock(_m);
				_queue.emplace(std::chrono::steady_clock::now() + a_delay, std::move(a_work));
				if (!_started) {
					_started = true;
					std::thread([this] { Run(); }).detach();
				}
			}
			_cv.notify_one();
		}

	private:
		void Run()
		{
			std::unique_lock lock(_m);
			for (;;) {
				if (_queue.empty()) {
					_cv.wait(lock);
					continue;
				}
				const auto due = _queue.begin()->first;
				if (std::chrono::steady_clock::now() < due) {
					_cv.wait_until(lock, due);
					continue;
				}
				auto work = std::move(_queue.begin()->second);
				_queue.erase(_queue.begin());
				lock.unlock();
				if (g_framePumpInstalled) {
					PostToFrame(std::move(work));
				} else {
					F4SE::GetTaskInterface()->AddTask(std::move(work));
				}
				lock.lock();
			}
		}

		std::mutex                                                                  _m;
		std::condition_variable                                                     _cv;
		std::multimap<std::chrono::steady_clock::time_point, std::function<void()>> _queue;
		bool                                                                        _started{ false };
	};

	// Posts only (no locks held by the caller matter): a_fn runs once a task lands on the main thread, else the task re-posts itself (a_onGiveUp runs after a_maxAttempts).
	void RunOnMainThread(const std::string& a_name, std::function<void()> a_fn, int a_maxAttempts, std::function<void()> a_onGiveUp = {}, int a_attempt = 1)
	{
		Timer::Get().Post(kMainThreadRetryDelay, [a_name, a_fn = std::move(a_fn), a_maxAttempts, a_onGiveUp = std::move(a_onGiveUp), a_attempt]() mutable {
			try {
				if (::GetCurrentThreadId() != g_mainThread) {
					if (a_attempt >= a_maxAttempts) {
						if (a_onGiveUp) {
							a_onGiveUp();
						}
						F4SE::log::warn("[{}] gave up after {} attempts", a_name, a_attempt);
						return;
					}
					RunOnMainThread(a_name, std::move(a_fn), a_maxAttempts, std::move(a_onGiveUp), a_attempt + 1);
					return;
				}
				a_fn();
			} catch (...) {
				F4SE::log::error("[{}] threw", a_name);
			}
		});
	}

	void RewalkAllLoaded(const char* a_path)
	{
		try {
			int actors = 0;
			int shapes = 0;
			if (auto player = RE::PlayerCharacter::GetPlayerHandle().get()) {
				++actors;
				shapes += WalkActor(player.get());
			}
			if (auto* lists = RE::ProcessLists::GetSingleton()) {
				// copy: the array can change while we walk
				std::vector<RE::ActorHandle> handles;
				for (auto& h : lists->highActorHandles) {
					handles.push_back(h);
				}
				for (auto& h : handles) {
					auto actor = h.get();
					if (!actor || actor->GetFormID() == 0x14) {
						continue;
					}
					++actors;
					shapes += WalkActor(actor.get());
				}
			}
			F4SE::log::debug("[{}] rewalk done: {} actors, {} skin-tint shapes (walk totals so far: {} geometries, {} with a shader material)", a_path, actors, shapes, g_dbgGeoms.load(), g_dbgShaders.load());
		} catch (...) {
			F4SE::log::error("[{}] rewalk threw", a_path);
		}
	}

	// Actor::Reset3D (RVA 0xC742F0 in 1.11.240, 0xD8A1F0 in 1.10.163): with queueReset set and a head flag it hands the rebuild to the game's own reset queue.
	// VR: ID 302888 -> 0xDDAD60 in 1.2.72, same prologue.
	constexpr std::array<std::uint8_t, 8> kReset3DPrologue{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C };
	constexpr auto                        kRefreshMinInterval = 1000ms;  // only guards against a double key press

	// Main thread only. Rebuilds the actor's 3D so every skin-tint material is created fresh and passes through our attach hook.
	void RefreshActor(RE::TESFormID a_id)
	{
		static const bool ok = PrologueMatches(REL::ID(302888).address(), kReset3DPrologue);
		static std::unordered_map<RE::TESFormID, std::chrono::steady_clock::time_point> last;
		if (!ok) {
			F4SE::log::error("[refresh] Reset3D prologue mismatch: refresh disabled");
			return;
		}
		auto* actor = RE::TESForm::GetFormByID<RE::Actor>(a_id);
		if (!actor || !actor->Get3D()) {
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		if (const auto it = last.find(a_id); it != last.end() && now - it->second < kRefreshMinInterval) {
			return;
		}
		last[a_id] = now;
		// reloadAll unloads and reloads the whole 3D, kHead puts the head in the rebuild so AttachSkinnedObject and OnHeadInitialized run again, queueReset leaves the rebuild to the game's reset queue.
		actor->Reset3D(true, static_cast<std::uint32_t>(RE::RESET_3D_FLAGS::kHead), true, 0);
	}

	void ForEachLoadedActor(const std::function<void(RE::Actor*)>& a_fn)
	{
		if (auto player = RE::PlayerCharacter::GetPlayerHandle().get()) {
			a_fn(player.get());
		}
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			std::vector<RE::ActorHandle> handles(lists->highActorHandles.begin(), lists->highActorHandles.end());
			for (auto& h : handles) {
				if (auto actor = h.get(); actor && actor->GetFormID() != 0x14) {
					a_fn(actor.get());
				}
			}
		}
	}

	// Effective factors of an actor's body and nape shapes under the current settings.
	struct ActorFactors
	{
		float body[3]{ 1, 1, 1 };
		float rear[3]{ 1, 1, 1 };
	};

	bool Differs(const ActorFactors& a, const ActorFactors& b)
	{
		for (int c = 0; c < 3; ++c) {
			if (std::abs(a.body[c] - b.body[c]) > 1e-4f || std::abs(a.rear[c] - b.rear[c]) > 1e-4f) {
				return true;
			}
		}
		return false;
	}

	std::map<RE::TESFormID, ActorFactors> SnapshotFactors()
	{
		std::map<RE::TESFormID, ActorFactors> out;
		ForEachLoadedActor([&](RE::Actor* a_actor) {
			auto* npc = BaseNpcOf(a_actor);
			if (!npc) {
				return;
			}
			const auto  color = SkinColor(TrackNpcOriginal(npc));
			const auto  body = ComputeLaw(npc, color, false);
			const auto  rear = ComputeLaw(npc, color, true);
			ActorFactors f;
			std::copy(std::begin(body.factor), std::end(body.factor), f.body);
			std::copy(std::begin(rear.factor), std::end(rear.factor), f.rear);
			out[a_actor->GetFormID()] = f;
		});
		return out;
	}

	// Main thread only. a_before is the snapshot taken before the settings reload; an actor is refreshed when its NPC skin field changed or its effective factors differ.
	void RefreshLoadedActors(const std::set<RE::TESFormID>& a_changed, const std::map<RE::TESFormID, ActorFactors>& a_before)
	{
		int refresh;
		{
			std::lock_guard lock(g_settingsLock);
			refresh = g_settings.refresh;
		}
		const auto              after = SnapshotFactors();
		std::set<RE::TESFormID> actors;
		int                     byField = 0;
		for (auto& [id, f] : after) {
			if (const auto it = a_before.find(id); it != a_before.end() && Differs(it->second, f)) {
				actors.insert(id);
			}
		}
		const auto factorCount = actors.size();
		ForEachLoadedActor([&](RE::Actor* a_actor) {
			if (auto* npc = BaseNpcOf(a_actor); npc && a_changed.contains(npc->GetFormID()) && actors.insert(a_actor->GetFormID()).second) {
				++byField;
			}
		});
		F4SE::log::debug("[refresh] {} loaded actors to refresh ({} by changed factors, {} more by changed NPC field), iRefresh={}", actors.size(), factorCount, byField, refresh);
		if (refresh > 0) {
			for (auto id : actors) {
				RefreshActor(id);
			}
		}
	}

	void ReloadAll()
	{
		F4SE::log::info("[reload] hotkey: re-reading {}", kIniPath);
		const auto before = SnapshotFactors();
		LoadSettings();
		const auto changed = ApplyAllNpcs();
		RewalkAllLoaded("reload");
		RefreshLoadedActors(changed, before);
	}

	// VR: MenuControls::RegisterHandler has no ID in the VR Address Library, and BSInputEventUser handlers registered through
	// community headers get no calls on the VR exe anyway. The reload key is polled instead: a thread reads the key state
	// every 50 ms (GetAsyncKeyState, so it fires with the game window focused or not) and queues the reload for the main thread.
	void ReloadKeyPollLoop()
	{
		bool wasDown = false;
		auto lastRun = std::chrono::steady_clock::now() - 10s;
		for (;;) {
			std::this_thread::sleep_for(50ms);
			const int key = g_reloadKey;
			if (key <= 0 || key > 0xFE) {
				wasDown = false;
				continue;
			}
			const bool down = (::GetAsyncKeyState(key) & 0x8000) != 0;
			const bool pressed = down && !wasDown;
			wasDown = down;
			if (!pressed) {
				continue;
			}
			const auto now = std::chrono::steady_clock::now();
			if (now - lastRun < 1000ms) {
				continue;
			}
			lastRun = now;
			RunOnMainThread("reload", [] { ReloadAll(); }, kMainThreadMaxAttempts);
		}
	}

	void RegisterReloadKey()
	{
		if (g_reloadKey <= 0) {
			F4SE::log::info("no reload key (iReloadKey=0)");
			return;
		}
		static std::once_flag once;
		std::call_once(once, [] { std::thread(ReloadKeyPollLoop).detach(); });
		F4SE::log::info("reload key poller started (iReloadKey={})", g_reloadKey.load());
	}

	void ScheduleActorRewalk(RE::TESFormID a_id)
	{
		Timer::Get().Post(kLoadedRewalkDelay, [a_id] {
			try {
				if (auto* actor = RE::TESForm::GetFormByID<RE::Actor>(a_id)) {
					WalkActor(actor);
				}
			} catch (...) {
				F4SE::log::error("[delayedRewalk] threw for {:08X}", a_id);
			}
		});
	}

	class LoadedSink : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent& a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
		{
			if (!a_event.loaded) {
				return RE::BSEventNotifyControl::kContinue;
			}
			auto* actor = RE::TESForm::GetFormByID<RE::Actor>(a_event.formID);
			if (!actor) {
				return RE::BSEventNotifyControl::kContinue;
			}
			auto* root = actor->Get3D();
			const int shapes = root ? WalkRoot(actor, root) : 0;
			if (shapes == 0) {
				ScheduleActorRewalk(a_event.formID);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	LoadedSink g_sink;

	// VR: TESObjectLoadedEvent::GetEventSource (ID 416662) is not in the VR Address Library 1.14.0, and CommonLibF4VR's getter aborts the
	// game on the missing ID. Its VR address comes from F4SE VR's GameEvents.h (0x42B8D0), one of the run of lazily built event source
	// getters at 0x4298F0..0x42CD70 (same table puts TESDeathEvent at 0x42AC50 and TESFurnitureEvent at 0x42B330, as the VR Address
	// Library does). Checked against the getter shape before it is called.
	RE::BSTEventSource<RE::TESObjectLoadedEvent>* ObjectLoadedEventSource()
	{
		constexpr std::uintptr_t                kRva = 0x42B8D0;
		constexpr std::array<std::uint8_t, 6>   kHead{ 0x48, 0x83, 0xEC, 0x28, 0x8B, 0x05 };  // sub rsp,28h; mov eax,[rip+guard]
		constexpr std::array<std::uint8_t, 3>   kTest{ 0xA8, 0x01, 0x75 };                    // test al,1; jne
		const auto                              addr = REL::Module::get().base() + kRva;
		const auto*                             p = reinterpret_cast<const std::uint8_t*>(addr);
		if (std::memcmp(p, kHead.data(), kHead.size()) != 0 || std::memcmp(p + 10, kTest.data(), kTest.size()) != 0) {
			F4SE::log::error("TESObjectLoadedEvent source getter mismatch at {:X}", addr);
			LogBytes("TESObjectLoadedEvent getter", addr);
			return nullptr;
		}
		using func_t = RE::BSTEventSource<RE::TESObjectLoadedEvent>*();
		return reinterpret_cast<func_t*>(addr)();
	}

	// Closing the looks menu writes the chosen skin tone as the player's new original and rebuilds the player once.
	class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event.menuName != "LooksMenu"sv) {
				return RE::BSEventNotifyControl::kContinue;
			}
			g_looksMenuOpen = a_event.opening;
			if (!a_event.opening) {
				RunOnMainThread("looksMenuClosed", [] {
					if (auto player = RE::PlayerCharacter::GetPlayerHandle().get()) {
						if (auto* npc = BaseNpcOf(player.get())) {
							ApplyNpc(npc);
						}
						RefreshActor(player->GetFormID());
						F4SE::log::info("looks menu closed: player re-tinted");
					}
				}, kMainThreadMaxAttempts);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	MenuSink g_menuSink;

	using AttachSkinnedObject_t = RE::NiAVObject*(RE::BipedAnim*, RE::NiNode*, RE::NiNode*, std::uint32_t, bool);
	AttachSkinnedObject_t* g_attachOriginal = nullptr;

	RE::NiAVObject* AttachSkinnedObject_Hook(RE::BipedAnim* a_biped, RE::NiNode* a_objectRoot, RE::NiNode* a_parent, std::uint32_t a_index, bool a_firstPerson)
	{
		auto* result = g_attachOriginal(a_biped, a_objectRoot, a_parent, a_index, a_firstPerson);
		try {
			if (a_biped && result) {
				auto  ref = a_biped->actorRef.get();
				auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (actor) {
					WalkRoot(actor, result, true);
				}
			}
		} catch (...) {
			F4SE::log::error("[attachHook] threw");
		}
		return result;
	}

	// Copies a_count whole instructions from a_addr into a trampoline stub followed by jmp [rip+0] back to a_addr + a_count.
	template <class T>
	T* MakeStub(std::uintptr_t a_addr, const std::uint8_t* a_bytes, std::size_t a_count)
	{
		auto& tramp = F4SE::GetTrampoline();
		auto* stub = static_cast<std::uint8_t*>(tramp.allocate(32));
		std::memcpy(stub, a_bytes, a_count);
		const std::uint8_t jmp[6]{ 0xFF, 0x25, 0, 0, 0, 0 };
		std::memcpy(stub + a_count, jmp, sizeof(jmp));
		const std::uint64_t back = a_addr + a_count;
		std::memcpy(stub + a_count + sizeof(jmp), &back, sizeof(back));
		return reinterpret_cast<T*>(stub);
	}

	void InstallAttachHook()
	{
		const auto addr = REL::ID(kAttachSkinnedObjectID).address();
		if (std::memcmp(reinterpret_cast<const void*>(addr), kAttachPrologue.data(), kAttachPrologue.size()) != 0) {
			F4SE::log::error("AttachSkinnedObject prologue mismatch at {:X}: hook NOT installed", addr);
			LogBytes("AttachSkinnedObject", addr);
			return;
		}
		g_attachOriginal = MakeStub<AttachSkinnedObject_t>(addr, kAttachPrologue.data(), kAttachPrologue.size());
		F4SE::GetTrampoline().write_branch<5>(addr, reinterpret_cast<std::uintptr_t>(&AttachSkinnedObject_Hook));
		F4SE::log::info("AttachSkinnedObject hook installed at {:X} (RVA {:X}, ID {})", addr, addr - REL::Module::get().base(), kAttachSkinnedObjectID);
	}

	// Actor::OnHeadInitialized (vtable slot 0x98, shared by Actor and PlayerCharacter), RVA 0xCA7420 in 1.11.240 and 0xDB9360 in 1.10.163.
	// It runs once the FaceGen head node exists (it flags that node through GetFaceNodeSkinned), so the nape is tinted before the renderer captures it.
	// VR: not in the VR Address Library. It is read from the Actor vtable (CommonLibF4VR VTABLE::Actor, slot 0x98): 0xE0A140 in 1.2.72, prologue
	// 40 53 48 83 EC 20, and it tail-jumps to the function that calls GetFaceNodeSkinned (vtable +0x408) and sets the head node flag, as on flat.
	// PlayerCharacter has its own slot 0x98 in VR (0xEF0310), which calls Actor's first and then sets a byte, so the inline hook covers the player too.
	constexpr std::size_t                 kHeadInitSlot = 0x98;
	constexpr std::array<std::uint8_t, 6> kHeadInitPrologue{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };  // push rbx; sub rsp,20h: whole instructions, none RIP-relative
	using HeadInit_t = void(RE::Actor*);
	HeadInit_t* g_headInitOriginal = nullptr;

	void HeadInit_Hook(RE::Actor* a_this)
	{
		g_headInitOriginal(a_this);
		try {
			if (!a_this) {
				return;
			}
			// BSFaceGenNiNode is only forward-declared in CommonLibF4; it derives NiNode as its first base.
			auto* head = reinterpret_cast<RE::NiNode*>(a_this->GetFaceNodeSkinned());
			if (head) {
				WalkRoot(a_this, head, true);
			}
		} catch (...) {
			F4SE::log::error("[faceGenHook] threw");
		}
	}

	void InstallHeadInitHook()
	{
		const auto vtbl = RE::VTABLE::Actor[0].address();
		const auto addr = reinterpret_cast<const std::uintptr_t*>(vtbl)[kHeadInitSlot];
		const auto text = REL::Module::get().segment(REL::Segment::text);
		if (addr < text.address() || addr >= text.address() + text.size()) {
			F4SE::log::error("OnHeadInitialized: Actor vtable slot {:X} points outside .text ({:X}): hook NOT installed", kHeadInitSlot, addr);
			return;
		}
		if (std::memcmp(reinterpret_cast<const void*>(addr), kHeadInitPrologue.data(), kHeadInitPrologue.size()) != 0) {
			F4SE::log::error("OnHeadInitialized prologue mismatch at {:X}: hook NOT installed", addr);
			LogBytes("OnHeadInitialized", addr);
			return;
		}
		g_headInitOriginal = MakeStub<HeadInit_t>(addr, kHeadInitPrologue.data(), kHeadInitPrologue.size());
		F4SE::GetTrampoline().write_branch<5>(addr, reinterpret_cast<std::uintptr_t>(&HeadInit_Hook));
		const std::uint8_t nop = 0x90;
		REL::safe_write(addr + 5, &nop, 1);  // the 6th stolen byte, so the entry never decodes into the middle of sub rsp
		F4SE::log::info("OnHeadInitialized hook installed at {:X} (RVA {:X}, Actor vtable slot {:X})", addr, addr - REL::Module::get().base(), kHeadInitSlot);
	}

	// The skin colour of an NPC, RVA 0x655910 in 1.11.240 and 0x5B58E0 in 1.10.163: (npc, NiColorA* out, tint entry, force, flag). For the player,
	// or forced, it re-derives the skin field from the skin tone tint entry. The re-tint (RVA 0x6E31F0 / 0x67B540) calls it forced, flag 0, after
	// OnHeadInitialized and paints the result on every skin-tint material of the actor's live 3D, nape included, made visible at once; with no
	// live 3D (a load) it paints nothing.
	// VR: skin colour 0x5A03A0, re-tint 0x669700 (its call at +0x2B, r9b=1, r8=0, flag 0, as on flat).
	constexpr std::uint64_t kSkinColorID = 134537;
	constexpr std::uint64_t kRetintID = 825803;
	using SkinColor_t = void(RE::TESNPC*, RE::NiColorA*, void*, bool, bool);
	SkinColor_t* g_skinColorOriginal = nullptr;

	void SkinColor_Hook(RE::TESNPC* a_npc, RE::NiColorA* a_out, void* a_entry, bool a_force, bool a_flag)
	{
		g_skinColorOriginal(a_npc, a_out, a_entry, a_force, a_flag);
		try {
			if (a_npc && a_out) {
				*a_out = Tinted(*a_out, ComputeLaw(a_npc, *a_out, false));
			}
		} catch (...) {
			F4SE::log::error("[retintHook] threw");
		}
	}

	void InstallRetintHook()
	{
		const auto retint = REL::ID(kRetintID).address();
		const auto site = FindCall(retint, REL::ID(kSkinColorID).address());
		if (!site) {
			F4SE::log::error("re-tint call to the skin colour not found in {:X}: hook NOT installed", retint);
			return;
		}
		g_skinColorOriginal = reinterpret_cast<SkinColor_t*>(F4SE::GetTrampoline().write_call<5>(site, reinterpret_cast<std::uintptr_t>(&SkinColor_Hook)));
		F4SE::log::info("re-tint hook installed at {:X} (ID {}+{:X})", site, kRetintID, site - retint);
	}

	// The main loop (RVA 0xC314F0 in 1.11.240, 0xD36D50 in 1.10.163) calls Main::Update (RVA 0xC339D0 / 0xD38E60, its only caller) once per frame
	// on the main thread, right after pumping the window messages; the call passes the Main singleton and nothing else.
	// VR: loop 0xD81860, Main::Update 0xD83DE0, the call at 0xD8191B. F4VR-CommonFramework mods (FRIK...) hook a different site, the call at
	// 0xD8405E inside Main::Update, so the two do not collide. If another plugin already rewrote this call, FindCall misses it and the
	// delayed work falls back to the F4SE task queue, as in 1.1.0.
	constexpr std::uint64_t kMainLoopID = 1125396;
	constexpr std::uint64_t kMainUpdateID = 633524;
	using MainUpdate_t = void(RE::Main*);
	MainUpdate_t* g_mainUpdateOriginal = nullptr;

	void MainUpdate_Hook(RE::Main* a_this)
	{
		g_mainUpdateOriginal(a_this);
		if (!g_frameJobsPending) {
			return;
		}
		std::vector<std::function<void()>> jobs;
		{
			std::lock_guard lock(g_frameLock);
			jobs.swap(g_frameJobs);
			g_frameJobsPending = false;
		}
		for (auto& job : jobs) {
			try {
				job();
			} catch (...) {
				F4SE::log::error("[framePump] threw");
			}
		}
	}

	void InstallMainUpdateHook()
	{
		const auto loop = REL::ID(kMainLoopID).address();
		const auto site = FindCall(loop, REL::ID(kMainUpdateID).address());
		if (!site) {
			F4SE::log::error("Main::Update call not found in {:X}: hook NOT installed, delayed work falls back to the F4SE task queue", loop);
			return;
		}
		g_mainUpdateOriginal = reinterpret_cast<MainUpdate_t*>(F4SE::GetTrampoline().write_call<5>(site, reinterpret_cast<std::uintptr_t>(&MainUpdate_Hook)));
		g_framePumpInstalled = true;
		F4SE::log::info("Main::Update hook installed at {:X} (ID {}+{:X})", site, kMainLoopID, site - loop);
	}

	void OnMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		if (a_msg->type == F4SE::MessagingInterface::kGameDataReady) {
			if (auto* src = ObjectLoadedEventSource()) {
				src->RegisterSink(&g_sink);
				F4SE::log::info("TESObjectLoadedEvent sink registered");
			} else {
				F4SE::log::error("TESObjectLoadedEvent source not available: actors loaded later are only tinted by the attach/head hooks");
			}
			RegisterReloadKey();
			if (auto* ui = RE::UI::GetSingleton()) {
				static_cast<RE::BSTEventSource<RE::MenuOpenCloseEvent>*>(ui)->RegisterSink(&g_menuSink);
				F4SE::log::info("looks menu guard registered");
			} else {
				F4SE::log::error("UI singleton is null: the looks menu guard is off");
			}
			ApplyAllNpcs();
		} else if (a_msg->type == F4SE::MessagingInterface::kPostLoadGame) {
			Timer::Get().Post(kPostLoadFieldDelay, [] {
				RunOnMainThread("postLoad NPC field pass", [] {
					auto changed = ApplyAllNpcs();
					// The game rewrites the player's field from the skin tone at every build: a changed field says nothing, and the player loads tinted.
					changed.erase(kPlayerBaseId);
					RefreshLoadedActors(changed, {});
				}, kMainThreadMaxAttempts);
			});
			for (auto d : kPostLoadDelays) {
				Timer::Get().Post(d, [] { RewalkAllLoaded("postLoadRewalk"); });
			}
		}
	}

	void InitLog()
	{
		auto path = F4SE::log::log_directory();
		if (!path) {
			return;
		}
		*path /= "HeadBodyColorMatch.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::debug);
		log->flush_on(spdlog::level::debug);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%Y-%m-%d %T.%e] [%L] %v");
	}
}

extern "C" __declspec(dllexport) constinit auto F4SEPlugin_Version = []() noexcept {
	F4SE::PluginVersionData data{};
	data.PluginName("HeadBodyColorMatch");
	data.PluginVersion(REL::Version(1, 1, 0, 4));
	data.AuthorName("mrtony (VR port: Axel)");
	data.UsesAddressLibrary(true);
	data.IsLayoutDependent(true);
	data.CompatibleVersions({ F4SE::RUNTIME_VR_1_2_72 });
	return data;
}();

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = "HeadBodyColorMatch";
	a_info->version = 0x01010004;
	if (a_f4se->IsEditor()) {
		return false;
	}
	// VR: F4SE VR reports 1.10.138 (the flat build VR was forked from) rather than 1.2.72 here, so accept both.
	if (const auto v = a_f4se->RuntimeVersion(); v != F4SE::RUNTIME_VR_1_2_72 && v != REL::Version(1, 10, 138)) {
		return false;
	}
	return true;
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se, false);
	InitLog();  // after Init: the log folder name needs F4SE's save folder name (port 3 wrote to My Games\\F4SE, without Fallout4VR)
	F4SE::AllocTrampoline(256);
	g_mainThread = ::GetCurrentThreadId();

	F4SE::log::info("Head Body Color Match 1.1.0 (VR port 4) loaded on runtime {}", REL::Module::get().version().string());
	LoadSettings();
	F4SE::GetMessagingInterface()->RegisterListener(OnMessage);
	InstallAttachHook();
	InstallHeadInitHook();
	InstallRetintHook();
	InstallMainUpdateHook();
	return true;
}
