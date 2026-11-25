#include "AudioMixerLive.h"

#include "AudioEffectRegistry.h"
#include "AudioSystem.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Reflection/TypeRegistry.h"
#include "Shalltear/Core/Time.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Shalltear {

	namespace AudioImpl {

		static_assert(static_cast<u8>(AudioMixerGroupClock::Wall) == static_cast<u8>(AudioBusClock::Wall) &&
						  static_cast<u8>(AudioMixerGroupClock::Gameplay) == static_cast<u8>(AudioBusClock::Gameplay),
					  "the public clock and the render thread's are one enumeration spelled twice");

		namespace {

			struct RegistryState {
				Vector<Handle<AudioMixer>> mixers{GetMemoryResource(MemTag::Audio)};
			};

			RegistryState* _registry = nullptr;

			RegistryState& Registry() {
				if (!_registry) {
					_registry = New<RegistryState>(MemTag::Audio);
				}
				return *_registry;
			}

			u32 FrameRampFrames() {
				const u32 rate = Audio::GetOutputSampleRate();
				if (rate == 0) {
					return AudioRenderer::BLOCK_FRAMES;
				}
				const f64 frames = static_cast<f64>(Time::GetUnscaledDeltaTime()) * static_cast<f64>(rate);
				const f64 lower = static_cast<f64>(AudioRenderer::BLOCK_FRAMES);
				const f64 upper = static_cast<f64>(rate) / 10.0;
				return static_cast<u32>(frames < lower ? lower : (frames > upper ? upper : frames));
			}

			u8 AimedRow(const AudioMixer& mixer, std::string_view group, std::string_view unit) {
				const i32 index = unit.empty() ? -1 : FindGroup(mixer, group);
				if (index < 0) {
					return NO_BUS_ROW;
				}
				const Vector<AudioMixer::Unit>& units = mixer.groups[static_cast<size_t>(index)].units;
				for (size_t at = 0; at < units.size(); ++at) {
					if ((units[at].kind == AudioMixerUnitKind::Receive || units[at].kind == AudioMixerUnitKind::Effect) && units[at].id == unit) {
						return static_cast<u8>(at);
					}
				}
				return NO_BUS_ROW;
			}

			bool IsAimedAt(const AudioMixer& mixer, std::string_view group, std::string_view id) {
				for (const AudioMixer::Group& sender : mixer.groups) {
					for (const AudioMixer::Unit& unit : sender.units) {
						if (unit.kind == AudioMixerUnitKind::Send && unit.target == group && !id.empty() && unit.targetUnit == id) {
							return true;
						}
					}
				}
				return false;
			}

			bool SameChain(const AudioMixer& mixer, const AudioMixer::Group& data, const AudioMixerLiveGroup& group) {
				if (data.units.size() != group.rows.size()) {
					return false;
				}
				for (size_t index = 0; index < data.units.size(); ++index) {
					const AudioMixer::Unit& unit = data.units[index];
					const AudioMixerLiveRow& row = group.rows[index];
					if (unit.kind != row.kind) {
						return false;
					}
					switch (unit.kind) {
					case AudioMixerUnitKind::Attenuation:
						break;
					case AudioMixerUnitKind::Send:
						if (unit.target != row.target || unit.targetUnit != row.targetUnit || AimedRow(mixer, unit.target, unit.targetUnit) != row.targetRow) {
							return false;
						}
						break;
					case AudioMixerUnitKind::Effect:
						if (unit.id != row.id || unit.effect != row.effect || unit.wetMixing != row.wetMixing) {
							return false;
						}
						[[fallthrough]];
					case AudioMixerUnitKind::Receive:
						if (unit.id != row.id || IsAimedAt(mixer, data.id, unit.id) != row.receives) {
							return false;
						}
						break;
					}
				}
				return true;
			}

			bool SameSends(const AudioMixer::Group& data, const AudioMixerLiveGroup& group) {
				size_t applied = 0;
				for (const AudioMixer::Unit& unit : data.units) {
					if (unit.kind != AudioMixerUnitKind::Send) {
						continue;
					}
					while (applied < group.rows.size() && group.rows[applied].kind != AudioMixerUnitKind::Send) {
						++applied;
					}
					if (applied >= group.rows.size() || group.rows[applied].target != unit.target) {
						return false;
					}
					++applied;
				}
				for (; applied < group.rows.size(); ++applied) {
					if (group.rows[applied].kind == AudioMixerUnitKind::Send) {
						return false;
					}
				}
				return true;
			}

			AudioBusRowKind RowKindOf(AudioMixerUnitKind kind) {
				switch (kind) {
				case AudioMixerUnitKind::Attenuation:
					return AudioBusRowKind::Fader;
				case AudioMixerUnitKind::Send:
					return AudioBusRowKind::Send;
				case AudioMixerUnitKind::Effect:
					return AudioBusRowKind::Effect;
				case AudioMixerUnitKind::Receive:
					return AudioBusRowKind::Receive;
				}
				return AudioBusRowKind::Fader;
			}

			void RetireRows(AudioMixerLiveGroup& group) {
				AudioSystem& system = GetAudioSystem();
				for (AudioMixerLiveRow& row : group.rows) {
					system.RetireEffect(row.instance);
					row.instance = nullptr;
					row.definition = nullptr;
				}
			}

			AudioMixerLiveGroup* FindLive(AudioMixerLive& live, std::string_view id) {
				for (AudioMixerLiveGroup& group : live.groups) {
					if (group.id == id) {
						return &group;
					}
				}
				return nullptr;
			}

			AudioMixerLiveParameter* FindParameter(AudioMixerLive& live, std::string_view group, AudioMixerParameter parameter, std::string_view target,
												   std::string_view effectParameter) {
				for (AudioMixerLiveParameter& entry : live.parameters) {
					if (entry.group == group && entry.parameter == parameter && entry.target == target && entry.effectParameter == effectParameter) {
						return &entry;
					}
				}
				return nullptr;
			}

			const AudioMixer::ExposedParameter* FindExposed(const AudioMixer& mixer, std::string_view name) {
				for (const AudioMixer::ExposedParameter& exposed : mixer.exposedParameters) {
					if (exposed.name == name) {
						return &exposed;
					}
				}
				return nullptr;
			}

			AudioBus ResolveOutput(AudioMixer& mixer, AudioMixerLive& live) {
				if (!mixer.outputMixer.IsValid()) {
					if (AudioMixer* held = live.output.Get()) {
						AssetDatabase::UnloadAsset(held);
					}
					live.output = {};
					return {};
				}
				AudioMixer* target = live.output.Get();
				if (!target || target->assetId != mixer.outputMixer) {
					if (target) {
						AssetDatabase::UnloadAsset(target);
					}
					live.output = {};
					target = nullptr;
					Asset* resolved = AssetDatabase::LoadAssetByGuid(mixer.outputMixer);
					if (resolved && resolved->GetAssetTypeId() == AudioMixer::GetReflectType()->name) {
						target = static_cast<AudioMixer*>(resolved);
						live.output = Handle<AudioMixer>(target);
					} else {
						if (resolved) {
							AssetDatabase::UnloadAsset(resolved);
						}
						if (!live.warnedOutput) {
							live.warnedOutput = true;
							SLT_LOG_WARN("Audio", "the mixer '{}' outputs to mixer {} (not a loadable mixer, top group feeds the listener)",
										 mixer.virtualPath, mixer.outputMixer.ToString());
						}
						return {};
					}
				}
				const AudioMixer* walk = target;
				for (u32 depth = 0; walk && depth < 64; ++depth) {
					if (walk == &mixer) {
						if (!live.warnedOutput) {
							live.warnedOutput = true;
							SLT_LOG_WARN("Audio", "the mixer '{}' outputs through a chain of mixers looping back to it (top group feeds the listener)",
										 mixer.virtualPath);
						}
						return {};
					}
					walk = walk->outputMixer.IsValid() ? static_cast<const AudioMixer*>(AssetDatabase::FindLoadedByGuid(walk->outputMixer)) : nullptr;
				}
				live.warnedOutput = false;
				return target->GetBus(mixer.outputGroup);
			}

			const AudioMixerLiveParameter* FindIn(const Vector<AudioMixerLiveParameter>& parameters, std::string_view group, AudioMixerParameter parameter,
												  std::string_view target, std::string_view effectParameter) {
				for (const AudioMixerLiveParameter& entry : parameters) {
					if (entry.group == group && entry.parameter == parameter && entry.target == target && entry.effectParameter == effectParameter) {
						return &entry;
					}
				}
				return nullptr;
			}

			IAudioEffect* MakeEffect(const AudioMixer& mixer, const Vector<AudioMixerLiveParameter>& parameters, std::string_view group,
									 const AudioMixer::Unit& unit, IAudioEffectDefinition& definition) {
				AudioSystem& system = GetAudioSystem();
				if (!system.GetRenderer().IsPrepared()) {
					return nullptr;
				}
				const std::span<const AudioParameterDefinition> table = definition.GetParameters();
				Vector<f32> values(table.size(), 0.0f, GetMemoryResource(MemTag::Audio));
				const u32 start = StartSnapshotIndex(mixer);
				for (size_t index = 0; index < table.size(); ++index) {
					const AudioMixerLiveParameter* entry = FindIn(parameters, group, AudioMixerParameter::EffectParameter, unit.id, table[index].name);
					const f32 value = entry ? entry->Effective()
											: SnapshotValue(mixer, start, group, AudioMixerParameter::EffectParameter, unit.id, table[index].name);
					values[index] = value < table[index].minimum ? table[index].minimum : (value > table[index].maximum ? table[index].maximum : value);
				}
				IAudioEffect* effect = definition.CreateEffect(system.MakeBusEffectContext({values.data(), values.size()}));
				if (!effect) {
					SLT_LOG_WARN("Audio", "the effect '{}' couldn't make an instance for '{}' of the mixer '{}' (sound passes through)",
								 unit.effect, unit.id, mixer.virtualPath);
				}
				return effect;
			}

			void MarkEffectSent(Vector<AudioMixerLiveParameter>& parameters, std::string_view group, std::string_view id) {
				for (AudioMixerLiveParameter& entry : parameters) {
					if (entry.parameter == AudioMixerParameter::EffectParameter && entry.group == group && entry.target == id) {
						entry.sent = entry.Effective();
						entry.everSent = true;
					}
				}
			}

			void Build(AudioMixer& mixer, AudioMixerLive& live) {
				AudioSystem& system = GetAudioSystem();
				if (!system.IsUp()) {
					return;
				}
				system.BeginBatch();

				const AudioBus outputBus = ResolveOutput(mixer, live);

				for (AudioMixerLiveGroup& group : live.groups) {
					if (FindGroup(mixer, group.id) < 0) {
						system.DestroyBus(group.bus);
						RetireRows(group);
						group.bus = {};
					}
				}
				live.groups.erase(std::remove_if(live.groups.begin(), live.groups.end(),
												 [](const AudioMixerLiveGroup& group) { return !group.bus.IsValid(); }),
								  live.groups.end());

				const AudioBusRowDesc faderOnly;
				for (AudioMixerLiveGroup& group : live.groups) {
					const AudioMixer::Group& data = mixer.groups[static_cast<size_t>(FindGroup(mixer, group.id))];
					if (data.parent != group.parent) {
						system.SetBusParent(group.bus, AudioBus{});
						group.parent = str("\x01");
					}
					if (!SameSends(data, group)) {
						system.SetBusChain(group.bus, &faderOnly, 1);
					}
				}

				Vector<u32> order{GetMemoryResource(MemTag::Audio)};
				for (size_t index = 0; index < mixer.groups.size(); ++index) {
					if (mixer.groups[index].parent.empty()) {
						order.push_back(static_cast<u32>(index));
					}
				}
				for (size_t head = 0; head < order.size(); ++head) {
					const str& id = mixer.groups[order[head]].id;
					for (size_t index = 0; index < mixer.groups.size(); ++index) {
						if (mixer.groups[index].parent == id) {
							order.push_back(static_cast<u32>(index));
						}
					}
				}

				const u32 start = StartSnapshotIndex(mixer);
				auto current = [&](std::string_view group, AudioMixerParameter parameter, std::string_view target) {
					if (const AudioMixerLiveParameter* entry = FindParameter(live, group, parameter, target, {})) {
						return entry->Effective();
					}
					return SnapshotValue(mixer, start, group, parameter, target, {});
				};

				for (u32 index : order) {
					const AudioMixer::Group& data = mixer.groups[index];
					if (FindLive(live, data.id)) {
						continue;
					}
					AudioBusDesc desc;
					if (data.parent.empty()) {
						desc.parent = outputBus;
					} else if (const AudioMixerLiveGroup* parent = FindLive(live, data.parent)) {
						desc.parent = parent->bus;
					}
					desc.clock = static_cast<AudioBusClock>(data.clock);
					desc.followTimeScale = data.followTimeScale;
					desc.fader = ToLinear(AudioMixerParameter::Volume, current(data.id, AudioMixerParameter::Volume, {}));
					desc.pitch = ToLinear(AudioMixerParameter::Pitch, current(data.id, AudioMixerParameter::Pitch, {}));
					AudioMixerLiveGroup group;
					group.id = data.id;
					group.bus = system.CreateBus(desc);
					group.parent = data.parent;
					group.clock = data.clock;
					group.followTimeScale = data.followTimeScale;
					group.rows.push_back(AudioMixerLiveRow{});
					if (!group.bus.IsValid()) {
						continue;
					}
					if (data.parent.empty()) {
						live.outputBus = outputBus;
					}
					live.groups.push_back(std::move(group));
				}

				for (u32 index : order) {
					const AudioMixer::Group& data = mixer.groups[index];
					AudioMixerLiveGroup* group = FindLive(live, data.id);
					if (!group) {
						continue;
					}
					if (data.parent.empty()) {
						if (group->parent != data.parent || live.outputBus != outputBus) {
							system.SetBusParent(group->bus, outputBus);
							live.outputBus = outputBus;
						}
						group->parent = data.parent;
						continue;
					}
					if (group->parent != data.parent) {
						const AudioMixerLiveGroup* parent = FindLive(live, data.parent);
						system.SetBusParent(group->bus, parent ? parent->bus : AudioBus{});
						group->parent = data.parent;
					}
				}

				Vector<AudioMixerLiveParameter> parameters{GetMemoryResource(MemTag::Audio)};
				for (const AudioMixer::Group& data : mixer.groups) {
					const AudioMixerLiveGroup* group = FindLive(live, data.id);
					auto adopt = [&](AudioMixerParameter kind, std::string_view target, std::string_view effectParameter, u32 row,
									 const IAudioEffectDefinition* definition) {
						if (FindIn(parameters, data.id, kind, target, effectParameter)) {
							return;
						}
						AudioMixerLiveParameter entry;
						if (const AudioMixerLiveParameter* previous = FindParameter(live, data.id, kind, target, effectParameter)) {
							entry = *previous;
							entry.overridden = entry.overridden && std::any_of(mixer.exposedParameters.begin(), mixer.exposedParameters.end(),
																			   [&](const AudioMixer::ExposedParameter& exposed) {
																				   return exposed.group == data.id && exposed.parameter == kind &&
																						  exposed.target == target && exposed.effectParameter == effectParameter;
																			   });
						} else {
							entry.group = data.id;
							entry.parameter = kind;
							entry.target = str(target);
							entry.effectParameter = str(effectParameter);
							entry.from = entry.to = entry.snapshot = SnapshotValue(mixer, start, data.id, kind, target, effectParameter);
							entry.sent = entry.snapshot;
							entry.everSent = kind != AudioMixerParameter::EffectParameter;
						}
						entry.bus = group ? group->bus : AudioBus{};
						entry.row = row;
						entry.index = NO_EFFECT_PARAMETER;
						if (kind == AudioMixerParameter::EffectParameter && definition) {
							entry.index = FindEffectParameter(*definition, effectParameter);
							if (entry.index != NO_EFFECT_PARAMETER) {
								entry.minimum = definition->GetParameters()[entry.index].minimum;
								entry.maximum = definition->GetParameters()[entry.index].maximum;
							} else {
								SLT_LOG_WARN("Audio", "the mixer '{}' sets '{}' of effect '{}' in '{}' (the effect doesn't have it)", mixer.virtualPath,
											 effectParameter, target, data.id);
							}
						}
						parameters.push_back(std::move(entry));
					};
					adopt(AudioMixerParameter::Volume, {}, {}, 0, nullptr);
					adopt(AudioMixerParameter::Pitch, {}, {}, 0, nullptr);
					for (size_t unit = 0; unit < data.units.size(); ++unit) {
						const AudioMixer::Unit& row = data.units[unit];
						const u32 at = static_cast<u32>(unit);
						if (row.kind == AudioMixerUnitKind::Send && !row.target.empty()) {
							adopt(AudioMixerParameter::SendLevel, row.target, {}, at, nullptr);
						}
						if (row.kind != AudioMixerUnitKind::Effect) {
							continue;
						}
						if (row.wetMixing) {
							adopt(AudioMixerParameter::WetLevel, row.id, {}, at, nullptr);
						}
						const IAudioEffectDefinition* definition = FindEffect(row.effect);
						if (definition) {
							for (const AudioParameterDefinition& parameter : definition->GetParameters()) {
								adopt(AudioMixerParameter::EffectParameter, row.id, parameter.name, at, definition);
							}
						}
						for (const AudioMixer::Snapshot& snapshot : mixer.snapshots) {
							for (const AudioMixer::Value& value : snapshot.values) {
								if (value.parameter == AudioMixerParameter::EffectParameter && value.group == data.id && value.target == row.id) {
									adopt(AudioMixerParameter::EffectParameter, row.id, value.effectParameter, at, definition);
								}
							}
						}
						for (const AudioMixer::ExposedParameter& exposed : mixer.exposedParameters) {
							if (exposed.parameter == AudioMixerParameter::EffectParameter && exposed.group == data.id && exposed.target == row.id) {
								adopt(AudioMixerParameter::EffectParameter, row.id, exposed.effectParameter, at, definition);
							}
						}
					}
				}
				live.parameters = std::move(parameters);

				for (u32 index : order) {
					const AudioMixer::Group& data = mixer.groups[index];
					AudioMixerLiveGroup* group = FindLive(live, data.id);
					if (!group) {
						continue;
					}
					if (!SameChain(mixer, data, *group)) {
						Vector<AudioMixerLiveRow> rows{GetMemoryResource(MemTag::Audio)};
						AudioBusRowDesc descs[MAX_BUS_ROWS];
						bool carried[MAX_BUS_ROWS]{};
						bool made[MAX_BUS_ROWS]{};
						const u32 count = static_cast<u32>(data.units.size());
						for (u32 at = 0; at < count; ++at) {
							const AudioMixer::Unit& unit = data.units[at];
							AudioMixerLiveRow row;
							row.kind = unit.kind;
							AudioBusRowDesc& desc = descs[at];
							desc = AudioBusRowDesc{};
							desc.kind = RowKindOf(unit.kind);
							if (unit.kind == AudioMixerUnitKind::Send) {
								row.target = unit.target;
								row.targetUnit = unit.targetUnit;
								row.targetRow = AimedRow(mixer, unit.target, unit.targetUnit);
								const AudioMixerLiveGroup* target = FindLive(live, unit.target);
								const bool feeds = target && (unit.targetUnit.empty() || row.targetRow != NO_BUS_ROW);
								desc.target = feeds ? target->bus : AudioBus{};
								desc.targetRow = row.targetRow;
								desc.level = ToLinear(AudioMixerParameter::SendLevel, current(data.id, AudioMixerParameter::SendLevel, unit.target));
							} else if (unit.kind == AudioMixerUnitKind::Receive) {
								row.id = unit.id;
								row.receives = IsAimedAt(mixer, data.id, unit.id);
								desc.receives = row.receives;
							} else if (unit.kind == AudioMixerUnitKind::Effect) {
								row.id = unit.id;
								row.effect = unit.effect;
								row.bypass = unit.bypass;
								row.wetMixing = unit.wetMixing;
								row.receives = IsAimedAt(mixer, data.id, unit.id);
								desc.receives = row.receives;
								for (size_t old = 0; old < group->rows.size() && old < MAX_BUS_ROWS; ++old) {
									const AudioMixerLiveRow& was = group->rows[old];
									if (!carried[old] && was.kind == AudioMixerUnitKind::Effect && was.instance && was.id == unit.id && was.effect == unit.effect) {
										row.instance = was.instance;
										row.definition = was.definition;
										carried[old] = true;
										break;
									}
								}
								if (!row.instance) {
									IAudioEffectDefinition* definition = FindEffect(unit.effect);
									row.instance = definition ? MakeEffect(mixer, live.parameters, data.id, unit, *definition) : nullptr;
									row.definition = row.instance ? definition : nullptr;
									made[at] = row.instance != nullptr;
								}
								desc.effect = row.instance;
								desc.bypass = unit.bypass;
								desc.wetMixing = unit.wetMixing;
								desc.level = unit.wetMixing ? ToLinear(AudioMixerParameter::WetLevel, current(data.id, AudioMixerParameter::WetLevel, unit.id)) : 1.0f;
							}
							rows.push_back(std::move(row));
						}
						if (system.SetBusChain(group->bus, descs, count)) {
							for (size_t old = 0; old < group->rows.size() && old < MAX_BUS_ROWS; ++old) {
								if (!carried[old]) {
									system.RetireEffect(group->rows[old].instance);
								}
							}
							for (u32 at = 0; at < count; ++at) {
								if (made[at]) {
									MarkEffectSent(live.parameters, data.id, rows[at].id);
								}
							}
							group->rows = std::move(rows);
						} else {
							SLT_LOG_ERROR("Audio", "the chain of '{}' in the mixer '{}' couldn't be applied", data.id, mixer.virtualPath);
							for (u32 at = 0; at < count; ++at) {
								if (made[at]) {
									rows[at].instance->Release();
								}
							}
						}
					} else {
						for (u32 at = 0; at < group->rows.size(); ++at) {
							const AudioMixer::Unit& unit = data.units[at];
							AudioMixerLiveRow& row = group->rows[at];
							if (row.kind != AudioMixerUnitKind::Effect) {
								continue;
							}
							if (row.bypass != unit.bypass) {
								system.SetBusRowBypass(group->bus, at, unit.bypass);
								row.bypass = unit.bypass;
							}
							if (row.instance) {
								continue;
							}
							IAudioEffectDefinition* definition = FindEffect(row.effect);
							IAudioEffect* instance = definition ? MakeEffect(mixer, live.parameters, data.id, unit, *definition) : nullptr;
							if (instance && system.SetBusRowEffect(group->bus, at, instance)) {
								row.instance = instance;
								row.definition = definition;
								MarkEffectSent(live.parameters, data.id, unit.id);
							} else if (instance) {
								instance->Release();
							}
						}
					}
					if (group->bypassEffects != data.bypassEffects) {
						system.SetBusBypassEffects(group->bus, data.bypassEffects);
						group->bypassEffects = data.bypassEffects;
					}

					if (group->clock != data.clock || group->followTimeScale != data.followTimeScale) {
						system.SetBusClock(group->bus, static_cast<AudioBusClock>(data.clock), data.followTimeScale);
						group->clock = data.clock;
						group->followTimeScale = data.followTimeScale;
					}
					if (group->mute != data.mute) {
						system.SetBusMute(group->bus, data.mute);
						group->mute = data.mute;
					}
					if (group->solo != data.solo) {
						system.SetBusSolo(group->bus, data.solo);
						group->solo = data.solo;
					}
				}

				live.built = true;
				++live.revision;
				PushMixer(live, AudioRenderer::BLOCK_FRAMES);

				system.EndBatch();
			}

			void Unbuild(AudioMixerLive& live, bool sendDestroys) {
				AudioSystem& system = GetAudioSystem();
				const bool destroyed = sendDestroys && live.built && system.IsUp();
				if (destroyed) {
					system.BeginBatch();
					for (AudioMixerLiveGroup& group : live.groups) {
						system.DestroyBus(group.bus);
						RetireRows(group);
					}
					system.EndBatch();
				} else {
					for (AudioMixerLiveGroup& group : live.groups) {
						for (AudioMixerLiveRow& row : group.rows) {
							system.ReleaseAtClose(row.instance);
							row.instance = nullptr;
							row.definition = nullptr;
						}
					}
				}
				live.groups.clear();
				live.outputBus = {};
				live.built = false;
				++live.revision;
				for (AudioMixerLiveParameter& parameter : live.parameters) {
					parameter.bus = {};
					parameter.everSent = false;
				}
			}

			void ReleaseHandedOut(AudioMixerLive& live) {
				Vector<AudioMixerKept> kept = std::move(live.handedOut);
				live.handedOut.clear();
				for (const AudioMixerKept& entry : kept) {
					AssetDatabase::UnloadAsset(entry.product);
				}
			}

			void SettleKept(AudioMixer& mixer, AudioMixerLive& live) {
				u32 releases = 0;
				for (const AudioMixerKept& entry : live.handedOut) {
					const bool heldElsewhere = entry.product->GetRefCount() > 1;
					if (heldElsewhere == *entry.holdsMixer) {
						continue;
					}
					if (!heldElsewhere) {
						*entry.holdsMixer = false;
						++releases;
						continue;
					}
					Asset* hold = AssetDatabase::LoadAssetByGuid(mixer.assetId);
					if (hold == &mixer) {
						*entry.holdsMixer = true;
					} else if (hold) {
						AssetDatabase::UnloadAsset(hold);
					}
				}
				for (; releases > 0; --releases) {
					AssetDatabase::UnloadAsset(&mixer);
				}
			}

			void Retire(AudioMixer& mixer) {
				AudioMixerLive* live = AudioMixerAccess::GetLive(mixer);
				if (!live || !live->live) {
					return;
				}
				Unbuild(*live, true);
				if (AudioMixer* held = live->output.Get()) {
					AssetDatabase::UnloadAsset(held);
				}
				live->output = {};
				ReleaseHandedOut(*live);
				live->live = false;
				live->transitioning = false;
				UnregisterMixer(mixer);
			}

			template <typename Product>
			Handle<Product> HandOut(AudioMixer& mixer, std::string_view key) {
				AudioMixerLive* live = AudioMixerAccess::GetLive(mixer);
				const Guid guid = AudioMixerAccess::FindProduct(mixer, key);
				if (!live || !live->live || !guid.IsValid()) {
					return {};
				}
				Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
				if (!asset) {
					return {};
				}
				if (asset->GetAssetTypeId() != Product::GetReflectType()->name) {
					AssetDatabase::UnloadAsset(asset);
					return {};
				}
				auto* product = static_cast<Product*>(asset);
				if (AudioMixerAccess::Keeps(mixer, *asset)) {
					AssetDatabase::UnloadAsset(asset);
				} else {
					AudioMixerAccess::KeptBy(*product, mixer, *live);
				}
				return Handle<Product>(product);
			}

		} // anonymous namespace

		void PushMixer(AudioMixerLive& live, u32 frames) {
			AudioSystem& system = GetAudioSystem();
			if (!live.built || !system.IsUp()) {
				return;
			}
			for (AudioMixerLiveParameter& parameter : live.parameters) {
				const f32 value = parameter.Effective();
				if (parameter.everSent && value == parameter.sent) {
					continue;
				}
				const f32 linear = ToLinear(parameter.parameter, value);
				bool sent = false;
				switch (parameter.parameter) {
				case AudioMixerParameter::Volume:
					sent = system.SetBusFader(parameter.bus, linear, frames);
					break;
				case AudioMixerParameter::Pitch:
					sent = system.SetBusPitch(parameter.bus, linear, frames);
					break;
				case AudioMixerParameter::SendLevel:
				case AudioMixerParameter::WetLevel:
					sent = system.SetBusRowLevel(parameter.bus, parameter.row, linear, frames);
					break;
				case AudioMixerParameter::EffectParameter:
					if (parameter.index == NO_EFFECT_PARAMETER) {
						continue;
					}
					sent = system.SetBusEffectParameter(parameter.bus, parameter.row, parameter.index,
														value < parameter.minimum ? parameter.minimum : (value > parameter.maximum ? parameter.maximum : value));
					break;
				}
				if (sent) {
					parameter.sent = value;
					parameter.everSent = true;
				}
			}
		}

		bool AudioMixerAccess::Keeps(const AudioMixer& mixer, const Asset& product) {
			if (!mixer._live) {
				return false;
			}
			for (const AudioMixerKept& kept : mixer._live->handedOut) {
				if (kept.product == &product) {
					return true;
				}
			}
			return false;
		}

		AudioBus ResolveGroupBus(Handle<AudioMixerGroup> group) {
			Object* object = ObjectRegistry::Resolve(group.GetInstanceId());
			if (!object || !TypeRegistry::IsDerivedFrom(object->GetType(), AudioMixerGroup::GetReflectType())) {
				return {};
			}
			return static_cast<AudioMixerGroup*>(object)->GetBus();
		}

		f32 ToLinear(AudioMixerParameter parameter, f32 value) {
			switch (parameter) {
			case AudioMixerParameter::Volume:
			case AudioMixerParameter::SendLevel:
				if (!(value > -80.0f)) {
					return 0.0f;
				}
				return std::pow(10.0f, (value < 20.0f ? value : 20.0f) / 20.0f);
			case AudioMixerParameter::Pitch:
				return value >= 0.01f ? (value <= 10.0f ? value : 10.0f) : 0.01f;
			case AudioMixerParameter::WetLevel:
				return value >= 0.0f ? (value <= 100.0f ? value / 100.0f : 1.0f) : 0.0f;
			case AudioMixerParameter::EffectParameter:
				return value;
			}
			return 1.0f;
		}

		AudioMixerLive& AudioMixerAccess::EnsureLive(AudioMixer& mixer) {
			if (!mixer._live) {
				mixer._live = New<AudioMixerLive>(MemTag::Audio);
			}
			return *mixer._live;
		}

		void AudioMixerAccess::DropLive(AudioMixer& mixer) {
			if (mixer._live) {
				Delete(mixer._live);
				mixer._live = nullptr;
			}
		}

		void RegisterMixer(AudioMixer& mixer) {
			const u64 id = mixer.GetInstanceId();
			Handle<AudioMixer>* reuse = nullptr;
			for (Handle<AudioMixer>& entry : Registry().mixers) {
				if (entry.GetInstanceId() == id) {
					return;
				}
				if (!reuse && entry.GetInstanceId() == 0) {
					reuse = &entry;
				}
			}
			if (reuse) {
				*reuse = Handle<AudioMixer>(&mixer);
				return;
			}
			Registry().mixers.push_back(Handle<AudioMixer>(&mixer));
		}

		void UnregisterMixer(AudioMixer& mixer) {
			if (!_registry) {
				return;
			}
			const u64 id = mixer.GetInstanceId();
			for (Handle<AudioMixer>& entry : _registry->mixers) {
				if (entry.GetInstanceId() == id) {
					entry = Handle<AudioMixer>{};
				}
			}
		}

		namespace {

			void Compact() {
				std::erase_if(_registry->mixers, [](const Handle<AudioMixer>& entry) { return !entry.IsValid(); });
			}

			f32 Walk(const AudioMixerLiveParameter& parameter, f32 t) {
				f32 share = t;
				switch (parameter.curve) {
				case AudioMixerTransitionType::Lerp:
					break;
				case AudioMixerTransitionType::Smoothstep:
					share = t * t * (3.0f - 2.0f * t);
					break;
				case AudioMixerTransitionType::Squared:
					share = t * t;
					break;
				case AudioMixerTransitionType::SquareRoot:
					share = std::sqrt(t);
					break;
				case AudioMixerTransitionType::BrickwallStart:
					share = t > 0.0f ? 1.0f : 0.0f;
					break;
				case AudioMixerTransitionType::BrickwallEnd:
					share = t >= 1.0f ? 1.0f : 0.0f;
					break;
				case AudioMixerTransitionType::Attenuation:
					if (parameter.parameter == AudioMixerParameter::Volume || parameter.parameter == AudioMixerParameter::SendLevel) {
						const f32 from = ToLinear(parameter.parameter, parameter.from);
						const f32 gain = from + (ToLinear(parameter.parameter, parameter.to) - from) * t;
						return gain > 1.0e-4f ? 20.0f * std::log10(gain) : -80.0f;
					}
					break;
				}
				return parameter.from + (parameter.to - parameter.from) * share;
			}

		} // anonymous namespace

		void ActivateMixers() {
			if (!_registry) {
				return;
			}
			for (size_t index = 0; index < _registry->mixers.size(); ++index) {
				const Handle<AudioMixer> entry = _registry->mixers[index];
				if (AudioMixer* mixer = entry.Get()) {
					Build(*mixer, AudioMixerAccess::EnsureLive(*mixer));
				}
			}
			Compact();
		}

		void DeactivateMixers() {
			if (!_registry) {
				return;
			}
			for (size_t index = 0; index < _registry->mixers.size(); ++index) {
				const Handle<AudioMixer> entry = _registry->mixers[index];
				if (AudioMixer* mixer = entry.Get()) {
					if (AudioMixerLive* live = AudioMixerAccess::GetLive(*mixer)) {
						Unbuild(*live, false);
					}
				}
			}
		}

		void UpdateMixers() {
			if (!_registry || _registry->mixers.empty()) {
				return;
			}
			AudioSystem& system = GetAudioSystem();
			const f32 scaled = Time::GetDeltaTime();
			const f32 unscaled = system.DidGameplayRun() ? Time::GetUnscaledDeltaTime() : 0.0f;
			const u32 frames = FrameRampFrames();

			for (size_t index = 0; index < _registry->mixers.size(); ++index) {
				const Handle<AudioMixer> entry = _registry->mixers[index];
				AudioMixer* mixer = entry.Get();
				AudioMixerLive* live = mixer ? AudioMixerAccess::GetLive(*mixer) : nullptr;
				if (!live) {
					continue;
				}
				SettleKept(*mixer, *live);
				if (!live->built) {
					Build(*mixer, *live);
					if (!live->built) {
						continue;
					}
				}

				const AudioBus output = ResolveOutput(*mixer, *live);
				if (output != live->outputBus) {
					for (const AudioMixerLiveGroup& group : live->groups) {
						if (group.parent.empty() && system.SetBusParent(group.bus, output)) {
							live->outputBus = output;
						}
					}
				}

				if (live->transitioning) {
					live->elapsed += mixer->updateMode == AudioMixerUpdateMode::Normal ? scaled : unscaled;
					const f32 t = live->duration > 0.0f ? std::min(live->elapsed / live->duration, 1.0f) : 1.0f;
					for (AudioMixerLiveParameter& parameter : live->parameters) {
						parameter.snapshot = t >= 1.0f ? parameter.to : Walk(parameter, t);
					}
					live->transitioning = t < 1.0f;
				}
				PushMixer(*live, frames);
			}
			Compact();
		}

		void ResetMixers() {
			if (!_registry) {
				return;
			}
			for (size_t index = 0; index < _registry->mixers.size(); ++index) {
				const Handle<AudioMixer> entry = _registry->mixers[index];
				AudioMixer* mixer = entry.Get();
				AudioMixerLive* live = mixer ? AudioMixerAccess::GetLive(*mixer) : nullptr;
				if (!live) {
					continue;
				}
				const u32 start = StartSnapshotIndex(*mixer);
				for (AudioMixerLiveParameter& parameter : live->parameters) {
					parameter.from = parameter.to = parameter.snapshot =
						SnapshotValue(*mixer, start, parameter.group, parameter.parameter, parameter.target, parameter.effectParameter);
					parameter.overridden = false;
				}
				live->heading.clear();
				live->headingWeights.clear();
				live->transitioning = false;
				PushMixer(*live, AudioRenderer::BLOCK_FRAMES);
			}
		}

		u32 CountLiveMixers() {
			if (!_registry) {
				return 0;
			}
			u32 count = 0;
			for (const Handle<AudioMixer>& entry : _registry->mixers) {
				count += entry.IsValid() ? 1u : 0u;
			}
			return count;
		}

		void OnEffectRegistered(std::string_view name) {
			if (!_registry) {
				return;
			}
			for (size_t index = 0; index < _registry->mixers.size(); ++index) {
				const Handle<AudioMixer> entry = _registry->mixers[index];
				AudioMixer* mixer = entry.Get();
				AudioMixerLive* live = mixer ? AudioMixerAccess::GetLive(*mixer) : nullptr;
				if (!live || !live->built) {
					continue;
				}
				const bool names = std::any_of(mixer->groups.begin(), mixer->groups.end(), [&](const AudioMixer::Group& group) {
					return std::any_of(group.units.begin(), group.units.end(),
									   [&](const AudioMixer::Unit& unit) { return unit.kind == AudioMixerUnitKind::Effect && unit.effect == name; });
				});
				if (names) {
					Build(*mixer, *live);
				}
			}
			Compact();
		}

		void DetachMixerEffects(IAudioEffectDefinition* definition, AudioDetachment& detachment) {
			if (!_registry || !definition) {
				return;
			}
			AudioSystem& system = GetAudioSystem();
			for (const Handle<AudioMixer>& entry : _registry->mixers) {
				AudioMixer* mixer = entry.Get();
				AudioMixerLive* live = mixer ? AudioMixerAccess::GetLive(*mixer) : nullptr;
				if (!live) {
					continue;
				}
				for (AudioMixerLiveGroup& group : live->groups) {
					for (u32 at = 0; at < group.rows.size(); ++at) {
						AudioMixerLiveRow& row = group.rows[at];
						if (row.definition != definition || !row.instance) {
							continue;
						}
						AudioCommand command;
						command.kind = AudioCommandKind::SetBusRowEffect;
						command.bus = AudioBusAccess::GetSlot(group.bus);
						command.busGeneration = AudioBusAccess::GetGeneration(group.bus);
						command.mode = static_cast<u8>(at);
						command.effect = nullptr;
						system.SendNow(command);
						detachment.effects.push_back(row.instance);
						detachment.posted = true;
						row.instance = nullptr;
						row.definition = nullptr;
					}
				}
				for (AudioMixerLiveParameter& parameter : live->parameters) {
					if (parameter.parameter != AudioMixerParameter::EffectParameter) {
						continue;
					}
					for (const AudioMixerLiveGroup& group : live->groups) {
						if (group.id == parameter.group && parameter.row < group.rows.size() && !group.rows[parameter.row].instance) {
							parameter.index = NO_EFFECT_PARAMETER;
						}
					}
				}
			}
		}

		void RemakeMixerEffects() {
			if (!_registry) {
				return;
			}
			AudioSystem& system = GetAudioSystem();
			static u32 madeRate = 0;
			static ChannelLayout madeLayout;
			const AudioRenderer& renderer = system.GetRenderer();
			const ChannelLayout& layout = renderer.GetLayout();
			bool same = madeRate == renderer.GetSampleRate() && madeLayout.count == layout.count;
			for (u32 channel = 0; same && channel < layout.count; ++channel) {
				same = madeLayout.positions[channel] == layout.positions[channel];
			}
			madeRate = renderer.GetSampleRate();
			madeLayout = layout;
			for (size_t index = 0; index < _registry->mixers.size(); ++index) {
				const Handle<AudioMixer> entry = _registry->mixers[index];
				AudioMixer* mixer = entry.Get();
				AudioMixerLive* live = mixer ? AudioMixerAccess::GetLive(*mixer) : nullptr;
				if (!live || !live->built) {
					continue;
				}
				if (same) {
					Build(*mixer, *live);
					continue;
				}
				for (AudioMixerLiveGroup& group : live->groups) {
					const i32 data = FindGroup(*mixer, group.id);
					if (data < 0) {
						continue;
					}
					for (u32 at = 0; at < group.rows.size(); ++at) {
						AudioMixerLiveRow& row = group.rows[at];
						if (!row.instance || !row.definition || at >= mixer->groups[static_cast<size_t>(data)].units.size()) {
							continue;
						}
						IAudioEffect* fresh = MakeEffect(*mixer, live->parameters, group.id, mixer->groups[static_cast<size_t>(data)].units[at], *row.definition);
						if (!system.SetBusRowEffect(group.bus, at, fresh)) {
							if (fresh) {
								fresh->Release();
							}
							continue;
						}
						row.instance->Release();
						row.instance = fresh;
						if (!fresh) {
							row.definition = nullptr;
						}
					}
				}
				Build(*mixer, *live);
			}
			Compact();
		}

		void ShutdownMixerRegistry() {
			if (!_registry) {
				return;
			}
			for (size_t index = 0; index < _registry->mixers.size(); ++index) {
				const Handle<AudioMixer> entry = _registry->mixers[index];
				if (AudioMixer* mixer = entry.Get()) {
					if (AudioMixerLive* live = AudioMixerAccess::GetLive(*mixer)) {
						live->live = false;
						live->built = false;
						live->transitioning = false;
						live->groups.clear();
						++live->revision;
						ReleaseHandedOut(*live);
					}
				}
			}
			Delete(_registry);
			_registry = nullptr;
		}

	} // namespace AudioImpl

	using namespace AudioImpl;

	AudioMixer::~AudioMixer() {
		Retire(*this);
		AudioMixerAccess::DropLive(*this);
	}

	void AudioMixer::OnLoad() {
		ApplyChanges();
	}

	void AudioMixer::OnUnload() {
		Retire(*this);
	}

	bool AudioMixer::ApplyChanges() {
		str error;
		if (!ValidateMixer(*this, error)) {
			SLT_LOG_ERROR("Audio", "the mixer '{}' can't be applied: {}", virtualPath, error);
			return false;
		}
		AudioMixerLive& live = AudioMixerAccess::EnsureLive(*this);
		if (!live.live) {
			live.live = true;
			RegisterMixer(*this);
		}
		Build(*this, live);
		return true;
	}

	AudioBus AudioMixer::GetBus(std::string_view group) const {
		if (!_live || !_live->built) {
			return {};
		}
		for (const AudioMixerLiveGroup& entry : _live->groups) {
			if (entry.id == group) {
				return entry.bus;
			}
		}
		return {};
	}

	Handle<AudioMixerSnapshot> AudioMixer::FindSnapshot(std::string_view name) {
		for (const Snapshot& snapshot : snapshots) {
			if (snapshot.name == name) {
				return HandOut<AudioMixerSnapshot>(*this, std::format("Snapshots/{}", snapshot.id));
			}
		}
		return {};
	}

	u32 AudioMixer::FindMatchingGroups(std::string_view subPath, Vector<Handle<AudioMixerGroup>>& out) {
		out.clear();
		for (const Group& group : groups) {
			str path = group.name;
			const Group* at = &group;
			for (size_t depth = 0; !at->parent.empty() && depth < groups.size(); ++depth) {
				const i32 parent = FindGroup(*this, at->parent);
				if (parent < 0) {
					break;
				}
				at = &groups[static_cast<size_t>(parent)];
				path = std::format("{}/{}", at->name, path);
			}
			if (path.find(subPath) == str::npos) {
				continue;
			}
			const Handle<AudioMixerGroup> found = HandOut<AudioMixerGroup>(*this, std::format("Groups/{}", group.id));
			if (found.IsValid()) {
				out.push_back(found);
			}
		}
		return static_cast<u32>(out.size());
	}

	bool AudioMixer::TransitionToSnapshots(std::span<const Handle<AudioMixerSnapshot>> snapshotAssets, std::span<const f32> weights,
										   f32 timeToReach) {
		Vector<std::string_view> ids{GetMemoryResource(MemTag::Audio)};
		ids.reserve(snapshotAssets.size());
		for (const Handle<AudioMixerSnapshot>& handle : snapshotAssets) {
			const AudioMixerSnapshot* snapshot = handle.Get();
			if (!snapshot || AudioMixerAccess::MixerOf(*snapshot) != assetId) {
				SLT_LOG_WARN("Audio", "the mixer '{}' was asked to transition to a snapshot it doesn't have", virtualPath);
				return false;
			}
			ids.push_back(snapshot->GetSnapshotId());
		}
		return TransitionToSnapshots(std::span<const std::string_view>(ids.data(), ids.size()), weights, timeToReach);
	}

	bool AudioMixer::TransitionToSnapshot(std::string_view snapshotId, f32 timeToReach) {
		const f32 weight = 1.0f;
		return TransitionToSnapshots(std::span<const std::string_view>(&snapshotId, 1), std::span<const f32>(&weight, 1), timeToReach);
	}

	bool AudioMixer::TransitionToSnapshots(std::span<const std::string_view> snapshotIds, std::span<const f32> weights, f32 timeToReach) {
		if (!_live || !_live->live || snapshotIds.empty() || snapshotIds.size() != weights.size()) {
			return false;
		}
		Vector<u32> indices{GetMemoryResource(MemTag::Audio)};
		f32 total = 0.0f;
		for (size_t index = 0; index < snapshotIds.size(); ++index) {
			u32 found = static_cast<u32>(snapshots.size());
			for (size_t candidate = 0; candidate < snapshots.size(); ++candidate) {
				if (snapshots[candidate].id == snapshotIds[index]) {
					found = static_cast<u32>(candidate);
					break;
				}
			}
			if (found == snapshots.size()) {
				SLT_LOG_WARN("Audio", "the mixer '{}' has no snapshot '{}'", virtualPath, snapshotIds[index]);
				return false;
			}
			indices.push_back(found);
			total += weights[index] > 0.0f ? weights[index] : 0.0f;
		}
		if (!(total > 0.0f)) {
			return false;
		}

		size_t lead = 0;
		for (size_t index = 1; index < indices.size(); ++index) {
			lead = weights[index] > weights[lead] ? index : lead;
		}
		const Vector<TransitionOverride>& overrides = snapshots[indices[lead]].transitions;

		for (AudioMixerLiveParameter& parameter : _live->parameters) {
			f32 target = 0.0f;
			for (size_t index = 0; index < indices.size(); ++index) {
				const f32 weight = weights[index] > 0.0f ? weights[index] : 0.0f;
				target += weight * SnapshotValue(*this, indices[index], parameter.group, parameter.parameter, parameter.target, parameter.effectParameter);
			}
			parameter.from = parameter.snapshot;
			parameter.to = target / total;
			parameter.curve = AudioMixerTransitionType::Lerp;
			for (const TransitionOverride& transition : overrides) {
				if (transition.group == parameter.group && transition.parameter == parameter.parameter && transition.target == parameter.target &&
					transition.effectParameter == parameter.effectParameter) {
					parameter.curve = transition.type;
					break;
				}
			}
		}
		std::swap(_live->heading, indices);
		_live->headingWeights.assign(weights.begin(), weights.end());
		_live->elapsed = 0.0f;
		_live->duration = timeToReach > 0.0f ? timeToReach : 0.0f;
		_live->transitioning = true;
		if (_live->duration == 0.0f) {
			for (AudioMixerLiveParameter& parameter : _live->parameters) {
				parameter.snapshot = parameter.to;
			}
			_live->transitioning = false;
			PushMixer(*_live, AudioRenderer::BLOCK_FRAMES);
		}
		return true;
	}

	bool AudioMixer::SetFloat(std::string_view name, f32 value) {
		const ExposedParameter* exposed = FindExposed(*this, name);
		AudioMixerLiveParameter* parameter =
			exposed && _live && _live->live ? FindParameter(*_live, exposed->group, exposed->parameter, exposed->target, exposed->effectParameter) : nullptr;
		if (!parameter) {
			return false;
		}
		parameter->override = value;
		parameter->overridden = true;
		PushMixer(*_live, AudioRenderer::BLOCK_FRAMES);
		return true;
	}

	bool AudioMixer::GetFloat(std::string_view name, f32& value) const {
		const ExposedParameter* exposed = FindExposed(*this, name);
		if (!exposed || !_live || !_live->live) {
			return false;
		}
		const AudioMixerLiveParameter* parameter = FindParameter(*_live, exposed->group, exposed->parameter, exposed->target, exposed->effectParameter);
		if (!parameter) {
			return false;
		}
		value = parameter->Effective();
		return true;
	}

	bool AudioMixer::ClearFloat(std::string_view name) {
		const ExposedParameter* exposed = FindExposed(*this, name);
		AudioMixerLiveParameter* parameter =
			exposed && _live && _live->live ? FindParameter(*_live, exposed->group, exposed->parameter, exposed->target, exposed->effectParameter) : nullptr;
		if (!parameter) {
			return false;
		}
		parameter->overridden = false;
		PushMixer(*_live, AudioRenderer::BLOCK_FRAMES);
		return true;
	}

} // namespace Shalltear
