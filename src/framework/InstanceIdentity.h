//
//	License type: BSD 3-Clause License
//	License copy: https://github.com/Telecominfraproject/wlan-cloud-ucentralgw/blob/master/LICENSE
//

#pragma once

#include <stdexcept>
#include <string>
#include <utility>

#include <Poco/UUIDGenerator.h>


namespace OpenWifi {

	/// InstanceIdentity captures the per-process runtime identity:
	///   - RuntimeIncarnationId: UUID generated once per process startup (always valid).
	///   - SlotId: optional operator-facing logical slot, read from config.
	///             Empty when no slot is configured.
	///   - InstanceId: composite identity (<slot>-<incarnation>) or just <incarnation>
	///                 when slot is empty.
	///
	/// Lifetime contract:
	///   RuntimeIncarnationId() - available immediately after construction; stable forever.
	///   SlotId()               - returns empty before Initialize() or when not configured.
	///   InstanceId()           - requires Initialize() to have been called first.
	///                            Throws std::logic_error before that point to prevent
	///                            callers from observing a value that will later change.
	///                            Once Initialize() returns, all three values are immutable.
	///
	/// Early-startup code that needs an identity before LoadMyConfig() completes
	/// must use RuntimeIncarnationId(), which is always stable.
	class InstanceIdentity {
	  public:
		InstanceIdentity() = delete;
		InstanceIdentity(const InstanceIdentity &) = delete;
		InstanceIdentity &operator=(const InstanceIdentity &) = delete;
		InstanceIdentity(InstanceIdentity &&) = delete;
		InstanceIdentity &operator=(InstanceIdentity &&) = delete;

		enum class ReloadResult {
			Unchanged,
			SlotChangedIgnored,
			InvalidSlotIgnored
		};

		/// Returns true if C is a valid hexadecimal digit [0-9 a-f A-F].
		static bool IsHexDigit(char C) noexcept {
			return (C >= '0' && C <= '9') ||
				   (C >= 'a' && C <= 'f') ||
				   (C >= 'A' && C <= 'F');
		}

		/// Validates strict UUID format: 8-4-4-4-12 hex digits separated by hyphens,
		/// version nibble == 4, and RFC 4122 variant bits (8, 9, a, or b at position 19).
		static bool IsValidUUID(const std::string &Uuid) noexcept {
			if (Uuid.size() != 36)
				return false;
			if (Uuid[8] != '-' || Uuid[13] != '-' || Uuid[18] != '-' || Uuid[23] != '-')
				return false;
			if (Uuid[14] != '4')
				return false;
			char Var = Uuid[19];
			if (Var != '8' && Var != '9' &&
				Var != 'a' && Var != 'b' &&
				Var != 'A' && Var != 'B')
				return false;
			for (size_t i = 0; i < 36; ++i) {
				if (i == 8 || i == 13 || i == 18 || i == 23)
					continue;
				if (!IsHexDigit(Uuid[i]))
					return false;
			}
			return true;
		}

		/// Generates a UUID string (RFC 4122 version 4).
		/// Delegates to Poco::UUIDGenerator, which is already a mandatory project
		/// dependency and handles entropy, RFC bit-setting, and formatting internally.
		static std::string GenerateUUID() {
			return Poco::UUIDGenerator::defaultGenerator().createRandom().toString();
		}


		/// Constructs with an existing runtime incarnation UUID (supplied by MicroService).
		/// Throws std::invalid_argument if the UUID string is malformed.
		explicit InstanceIdentity(std::string RuntimeIncarnationId)
			: RuntimeIncarnationId_(std::move(RuntimeIncarnationId)) {
			if (!IsValidUUID(RuntimeIncarnationId_))
				throw std::invalid_argument(
					"InstanceIdentity: RuntimeIncarnationId must be a valid UUID");
		}

		/// Always returns the UUID generated at process startup. Stable for the process lifetime.
		[[nodiscard]] const std::string &RuntimeIncarnationId() const noexcept {
			return RuntimeIncarnationId_;
		}

		/// Returns the configured slot label, or empty when uninitialized / not configured.
		[[nodiscard]] const std::string &SlotId() const noexcept {
			return SlotId_;
		}

		/// Returns the composite instance identity (<slot>-<incarnation> or <incarnation>).
		/// Requires Initialize() to have been called first. Throws std::logic_error before that
		/// to enforce value stability: the value must not change within one process lifetime.
		/// Early-startup code must use RuntimeIncarnationId() instead.
		[[nodiscard]] const std::string &InstanceId() const {
			if (!Initialized_)
				throw std::logic_error(
					"InstanceIdentity::InstanceId() called before Initialize(). "
					"Use RuntimeIncarnationId() for pre-config-load identity.");
			return InstanceId_;
		}

		[[nodiscard]] bool IsInitialized() const noexcept {
			return Initialized_;
		}

		/// Validates, trims, and sanitizes a candidate slot ID string.
		/// Returns the trimmed value on success (empty string == no slot configured).
		/// Throws std::invalid_argument if any character is outside [A-Za-z0-9._-].
		static std::string ValidateSlotId(const std::string &Raw) {
			auto First = Raw.find_first_not_of(" \t\r\n");
			if (First == std::string::npos)
				return {};
			auto Last = Raw.find_last_not_of(" \t\r\n");
			std::string Trimmed = Raw.substr(First, Last - First + 1);

			if (Trimmed.size() > 64) {
				throw std::invalid_argument(
					"openwifi.system.slot.id length (" + std::to_string(Trimmed.size()) +
					") exceeds maximum allowed (64 characters)");
			}

			for (char C : Trimmed) {
				if (!((C >= 'A' && C <= 'Z') ||
					  (C >= 'a' && C <= 'z') ||
					  (C >= '0' && C <= '9') ||
					  C == '.' || C == '_' || C == '-')) {
					throw std::invalid_argument(
						"openwifi.system.slot.id contains invalid character '" +
						std::string(1, C) +
						"'. Allowed: A-Z a-z 0-9 . _ -");
				}
			}
			return Trimmed;
		}

		/// Locks in SlotId and the composite InstanceId on the first call.
		/// Subsequent calls are no-ops (returns false). Identity is immutable once set.
		/// If RawSlotId contains unsupported formatting, falls back to empty slot ("no slot")
		/// and populates WarningMessage if provided.
		bool Initialize(const std::string &RawSlotId, std::string *WarningMessage = nullptr) {
			if (!Initialized_) {
				try {
					SlotId_ = ValidateSlotId(RawSlotId);
				} catch (const std::invalid_argument &E) {
					if (WarningMessage) {
						*WarningMessage =
							"openwifi.system.slot.id has an invalid format (" +
							std::string(E.what()) +
							"). Proceeding with unslotted runtime identity.";
					}
					SlotId_ = "";
				}
				InstanceId_ = SlotId_.empty()
					? RuntimeIncarnationId_
					: SlotId_ + "-" + RuntimeIncarnationId_;
				Initialized_ = true;
				return true;
			}
			return false;
		}

		/// Called during a configuration reload to detect slot changes.
		/// The identity is immutable once initialized -- any change is logged and ignored.
		/// Populates WarningMessage (when non-null) with a human-readable explanation.
		/// Returns Unchanged / SlotChangedIgnored / InvalidSlotIgnored.
		/// Throws std::logic_error if called before Initialize().
		ReloadResult HandleConfigReload(const std::string &NewConfiguredSlot,
										std::string *WarningMessage = nullptr) const {
			if (!Initialized_)
				throw std::logic_error(
					"InstanceIdentity::HandleConfigReload() called before Initialize()");
			try {
				auto Normalized = ValidateSlotId(NewConfiguredSlot);
				if (Normalized != SlotId_) {
					if (WarningMessage) {
						const std::string &OldSlot = SlotId_.empty() ? "<not configured>" : SlotId_;
						const std::string &NewSlot = Normalized.empty() ? "<not configured>" : Normalized;
						*WarningMessage =
							"openwifi.system.slot.id changed from '" + OldSlot +
							"' to '" + NewSlot +
							"' in config reload - ignored. Changing slot identity "
							"requires a process restart.";
					}
					return ReloadResult::SlotChangedIgnored;
				}
				return ReloadResult::Unchanged;
			} catch (const std::invalid_argument &E) {
				if (WarningMessage) {
					*WarningMessage =
						"openwifi.system.slot.id has an invalid value in config reload - "
						"ignored (" + std::string(E.what()) +
						"). Current slot identity unchanged.";
				}
				return ReloadResult::InvalidSlotIgnored;
			}
		}

	  private:
		std::string RuntimeIncarnationId_;
		std::string SlotId_;
		std::string InstanceId_;
		bool Initialized_ = false;
	};

} // namespace OpenWifi
