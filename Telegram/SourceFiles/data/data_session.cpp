/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_session.h"

#include "observer_peer.h"
#include "auth_session.h"
#include "apiwrap.h"
#include "messenger.h"
#include "core/crash_reports.h" // for CrashReports::SetAnnotation
#include "ui/image/image.h"
#include "export/export_controller.h"
#include "export/view/export_view_panel_controller.h"
#include "window/notifications_manager.h"
#include "history/history.h"
#include "history/history_item_components.h"
#include "history/media/history_media.h"
#include "history/view/history_view_element.h"
#include "inline_bots/inline_bot_layout_item.h"
#include "storage/localstorage.h"
#include "storage/storage_encrypted_file.h"
#include "boxes/abstract_box.h"
#include "passport/passport_form_controller.h"
#include "window/themes/window_theme.h"
#include "lang/lang_keys.h" // for lang(lng_deleted) in user name.
#include "data/data_media_types.h"
#include "data/data_feed.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_user.h"
#include "data/data_file_origin.h"
#include "data/data_photo.h"
#include "data/data_document.h"
#include "data/data_web_page.h"
#include "data/data_game.h"
#include "data/data_poll.h"
#include "styles/style_boxes.h" // for st::backgroundSize

namespace Data {
namespace {

constexpr auto kMaxNotifyCheckDelay = 24 * 3600 * TimeMs(1000);
constexpr auto kMaxWallpaperSize = 10 * 1024 * 1024;

using ViewElement = HistoryView::Element;

// s: box 100x100
// m: box 320x320
// x: box 800x800
// y: box 1280x1280
// w: box 2560x2560 // if loading this fix HistoryPhoto::updateFrom
// a: crop 160x160
// b: crop 320x320
// c: crop 640x640
// d: crop 1280x1280
const auto ThumbLevels = QByteArray::fromRawData("isambcxydw", 10);
const auto MediumLevels = QByteArray::fromRawData("mbcxasydwi", 10);
const auto FullLevels = QByteArray::fromRawData("yxwmsdcbai", 10);

void UpdateImage(ImagePtr &old, ImagePtr now) {
	if (now->isNull()) {
		return;
	}
	if (old->isNull()) {
		old = now;
	} else if (old->isDelayedStorageImage()) {
		const auto location = now->location();
		if (!location.isNull()) {
			old->setDelayedStorageLocation(Data::FileOrigin(), location);
		}
	}
}

void CheckForSwitchInlineButton(not_null<HistoryItem*> item) {
	if (item->out() || !item->hasSwitchInlineButton()) {
		return;
	}
	if (const auto user = item->history()->peer->asUser()) {
		if (!user->botInfo || !user->botInfo->inlineReturnPeerId) {
			return;
		}
		if (const auto markup = item->Get<HistoryMessageReplyMarkup>()) {
			for (const auto &row : markup->rows) {
				for (const auto &button : row) {
					using ButtonType = HistoryMessageMarkupButton::Type;
					if (button.type == ButtonType::SwitchInline) {
						Notify::switchInlineBotButtonReceived(
							QString::fromUtf8(button.data));
						return;
					}
				}
			}
		}
	}
}

// We should get a full restriction in "{full}: {reason}" format and we
// need to find an "-all" tag in {full}, otherwise ignore this restriction.
QString ExtractUnavailableReason(const QString &restriction) {
	const auto fullEnd = restriction.indexOf(':');
	if (fullEnd <= 0) {
		return QString();
	}

	// {full} is in "{type}-{tag}-{tag}-{tag}" format
	// if we find "all" tag we return the restriction string
	const auto typeTags = restriction.mid(0, fullEnd).split('-').mid(1);
#ifndef OS_MAC_STORE
	const auto restrictionApplies = typeTags.contains(qsl("all"));
#else // OS_MAC_STORE
	const auto restrictionApplies = typeTags.contains(qsl("all"))
		|| typeTags.contains(qsl("ios"));
#endif // OS_MAC_STORE
	if (restrictionApplies) {
		return restriction.midRef(fullEnd + 1).trimmed().toString();
	}
	return QString();
}

MTPPhotoSize FindDocumentThumb(const MTPDdocument &data) {
	const auto area = [](const MTPPhotoSize &size) {
		static constexpr auto kInvalid = std::numeric_limits<int>::max();
		return size.match([](const MTPDphotoSizeEmpty &) {
			return kInvalid;
		}, [](const MTPDphotoStrippedSize &) {
			return kInvalid;
		}, [](const auto &data) {
			return (data.vw.v >= 90 || data.vh.v >= 90)
				? (data.vw.v * data.vh.v)
				: kInvalid;
		});
	};
	const auto &thumbs = data.vthumbs.v;
	const auto i = ranges::max_element(thumbs, std::greater<>(), area);
	return (i != thumbs.end())
		? (*i)
		: MTPPhotoSize(MTP_photoSizeEmpty(MTP_string("")));
}

} // namespace

Session::Session(not_null<AuthSession*> session)
: _session(session)
, _cache(Messenger::Instance().databases().get(
	Local::cachePath(),
	Local::cacheSettings()))
, _selfDestructTimer([=] { checkSelfDestructItems(); })
, _a_sendActions(animation(this, &Session::step_typings))
, _groups(this)
, _unmuteByFinishedTimer([=] { unmuteByFinished(); }) {
	_cache->open(Local::cacheKey());

	setupContactViewsViewer();
	setupChannelLeavingViewer();
}

void Session::clear() {
	_sendActions.clear();

	for (const auto &[peerId, history] : _histories) {
		history->unloadBlocks();
	}
	App::historyClearMsgs();
	_histories.clear();

	App::historyClearItems();
}

not_null<PeerData*> Session::peer(PeerId id) {
	const auto i = _peers.find(id);
	if (i != _peers.cend()) {
		return i->second.get();
	}
	auto result = [&]() -> std::unique_ptr<PeerData> {
		if (peerIsUser(id)) {
			return std::make_unique<UserData>(this, id);
		} else if (peerIsChat(id)) {
			return std::make_unique<ChatData>(this, id);
		} else if (peerIsChannel(id)) {
			return std::make_unique<ChannelData>(this, id);
		}
		Unexpected("Peer id type.");
	}();

	result->input = MTPinputPeer(MTP_inputPeerEmpty());
	return _peers.emplace(id, std::move(result)).first->second.get();
}

not_null<UserData*> Session::user(UserId id) {
	return peer(peerFromUser(id))->asUser();
}

not_null<ChatData*> Session::chat(ChatId id) {
	return peer(peerFromChat(id))->asChat();
}

not_null<ChannelData*> Session::channel(ChannelId id) {
	return peer(peerFromChannel(id))->asChannel();
}

PeerData *Session::peerLoaded(PeerId id) const {
	const auto i = _peers.find(id);
	if (i == end(_peers)) {
		return nullptr;
	} else if (i->second->loadedStatus != PeerData::FullLoaded) {
		return nullptr;
	}
	return i->second.get();
}

UserData *Session::userLoaded(UserId id) const {
	if (const auto peer = peerLoaded(peerFromUser(id))) {
		return peer->asUser();
	}
	return nullptr;
}

ChatData *Session::chatLoaded(ChatId id) const {
	if (const auto peer = peerLoaded(peerFromChat(id))) {
		return peer->asChat();
	}
	return nullptr;
}

ChannelData *Session::channelLoaded(ChannelId id) const {
	if (const auto peer = peerLoaded(peerFromChannel(id))) {
		return peer->asChannel();
	}
	return nullptr;
}

not_null<UserData*> Session::user(const MTPUser &data) {
	const auto result = user(data.match([](const auto &data) {
		return data.vid.v;
	}));
	auto minimal = false;
	const MTPUserStatus *status = 0, emptyStatus = MTP_userStatusEmpty();

	Notify::PeerUpdate update;
	using UpdateFlag = Notify::PeerUpdate::Flag;
	data.match([&](const MTPDuserEmpty &data) {
		const auto canShareThisContact = result->canShareThisContactFast();

		result->input = MTP_inputPeerUser(data.vid, MTP_long(0));
		result->inputUser = MTP_inputUser(data.vid, MTP_long(0));
		result->setName(lang(lng_deleted), QString(), QString(), QString());
		result->setPhoto(MTP_userProfilePhotoEmpty());
		//result->setFlags(MTPDuser_ClientFlag::f_inaccessible | 0);
		result->setFlags(MTPDuser::Flag::f_deleted);
		if (!result->phone().isEmpty()) {
			result->setPhone(QString());
			update.flags |= UpdateFlag::UserPhoneChanged;
		}
		result->setBotInfoVersion(-1);
		status = &emptyStatus;
		result->setContactStatus(UserData::ContactStatus::PhoneUnknown);
		if (canShareThisContact != result->canShareThisContactFast()) {
			update.flags |= UpdateFlag::UserCanShareContact;
		}
	}, [&](const MTPDuser &data) {
		minimal = data.is_min();

		const auto canShareThisContact = result->canShareThisContactFast();
		if (minimal) {
			const auto mask = 0
				//| MTPDuser_ClientFlag::f_inaccessible
				| MTPDuser::Flag::f_deleted;
			result->setFlags((result->flags() & ~mask) | (data.vflags.v & mask));
		} else {
			result->setFlags(data.vflags.v);
			if (data.is_self()) {
				result->input = MTP_inputPeerSelf();
				result->inputUser = MTP_inputUserSelf();
			} else if (!data.has_access_hash()) {
				result->input = MTP_inputPeerUser(data.vid, MTP_long(result->accessHash()));
				result->inputUser = MTP_inputUser(data.vid, MTP_long(result->accessHash()));
			} else {
				result->input = MTP_inputPeerUser(data.vid, data.vaccess_hash);
				result->inputUser = MTP_inputUser(data.vid, data.vaccess_hash);
			}
			result->setUnavailableReason(data.is_restricted()
				? ExtractUnavailableReason(qs(data.vrestriction_reason))
				: QString());
		}
		if (data.is_deleted()) {
			if (!result->phone().isEmpty()) {
				result->setPhone(QString());
				update.flags |= UpdateFlag::UserPhoneChanged;
			}
			result->setName(lang(lng_deleted), QString(), QString(), QString());
			result->setPhoto(MTP_userProfilePhotoEmpty());
			status = &emptyStatus;
		} else {
			// apply first_name and last_name from minimal user only if we don't have
			// local values for first name and last name already, otherwise skip
			bool noLocalName = result->firstName.isEmpty() && result->lastName.isEmpty();
			QString fname = (!minimal || noLocalName) ? (data.has_first_name() ? TextUtilities::SingleLine(qs(data.vfirst_name)) : QString()) : result->firstName;
			QString lname = (!minimal || noLocalName) ? (data.has_last_name() ? TextUtilities::SingleLine(qs(data.vlast_name)) : QString()) : result->lastName;

			QString phone = minimal ? result->phone() : (data.has_phone() ? qs(data.vphone) : QString());
			QString uname = minimal ? result->username : (data.has_username() ? TextUtilities::SingleLine(qs(data.vusername)) : QString());

			const auto phoneChanged = (result->phone() != phone);
			if (phoneChanged) {
				result->setPhone(phone);
				update.flags |= UpdateFlag::UserPhoneChanged;
			}
			const auto nameChanged = (result->firstName != fname)
				|| (result->lastName != lname);

			auto showPhone = !isServiceUser(result->id)
				&& !data.is_self()
				&& !data.is_contact()
				&& !data.is_mutual_contact();
			auto showPhoneChanged = !isServiceUser(result->id)
				&& !data.is_self()
				&& ((showPhone
					&& result->contactStatus() == UserData::ContactStatus::Contact)
					|| (!showPhone
						&& result->contactStatus() == UserData::ContactStatus::CanAdd));
			if (minimal) {
				showPhoneChanged = false;
				showPhone = !isServiceUser(result->id)
					&& (result->id != _session->userPeerId())
					&& (result->contactStatus() == UserData::ContactStatus::CanAdd);
			}

			// see also Local::readPeer

			const auto pname = (showPhoneChanged || phoneChanged || nameChanged)
				? ((showPhone && !phone.isEmpty())
					? App::formatPhone(phone)
					: QString())
				: result->nameOrPhone;

			if (!minimal && data.is_self() && uname != result->username) {
				CrashReports::SetAnnotation("Username", uname);
			}
			result->setName(fname, lname, pname, uname);
			if (data.has_photo()) {
				result->setPhoto(data.vphoto);
			} else {
				result->setPhoto(MTP_userProfilePhotoEmpty());
			}
			if (data.has_access_hash()) {
				result->setAccessHash(data.vaccess_hash.v);
			}
			status = data.has_status() ? &data.vstatus : &emptyStatus;
		}
		if (!minimal) {
			if (data.has_bot_info_version()) {
				result->setBotInfoVersion(data.vbot_info_version.v);
				result->botInfo->readsAllHistory = data.is_bot_chat_history();
				if (result->botInfo->cantJoinGroups != data.is_bot_nochats()) {
					result->botInfo->cantJoinGroups = data.is_bot_nochats();
					update.flags |= UpdateFlag::BotCanAddToGroups;
				}
				result->botInfo->inlinePlaceholder = data.has_bot_inline_placeholder() ? '_' + qs(data.vbot_inline_placeholder) : QString();
			} else {
				result->setBotInfoVersion(-1);
			}
			result->setContactStatus((data.is_contact() || data.is_mutual_contact())
				? UserData::ContactStatus::Contact
				: result->phone().isEmpty()
				? UserData::ContactStatus::PhoneUnknown
				: UserData::ContactStatus::CanAdd);
		}

		if (canShareThisContact != result->canShareThisContactFast()) {
			update.flags |= UpdateFlag::UserCanShareContact;
		}
	});

	if (minimal) {
		if (result->loadedStatus == PeerData::NotLoaded) {
			result->loadedStatus = PeerData::MinimalLoaded;
		}
	} else if (result->loadedStatus != PeerData::FullLoaded
		&& (!result->isSelf() || !result->phone().isEmpty())) {
		result->loadedStatus = PeerData::FullLoaded;
	}

	if (status && !minimal) {
		const auto oldOnlineTill = result->onlineTill;
		const auto newOnlineTill = ApiWrap::OnlineTillFromStatus(
			*status,
			oldOnlineTill);
		if (oldOnlineTill != newOnlineTill) {
			result->onlineTill = newOnlineTill;
			update.flags |= UpdateFlag::UserOnlineChanged;
		}
	}

	if (result->contactStatus() == UserData::ContactStatus::PhoneUnknown
		&& !result->phone().isEmpty()
		&& !result->isSelf()) {
		result->setContactStatus(UserData::ContactStatus::CanAdd);
	}
	if (App::main()) {
		if (update.flags) {
			update.peer = result;
			Notify::peerUpdatedDelayed(update);
		}
	}
	return result;
}

not_null<PeerData*> Session::chat(const MTPChat &data) {
	const auto result = data.match([&](const MTPDchat &data) {
		return peer(peerFromChat(data.vid.v));
	}, [&](const MTPDchatForbidden &data) {
		return peer(peerFromChat(data.vid.v));
	}, [&](const MTPDchatEmpty &data) {
		return peer(peerFromChat(data.vid.v));
	}, [&](const MTPDchannel &data) {
		return peer(peerFromChannel(data.vid.v));
	}, [&](const MTPDchannelForbidden &data) {
		return peer(peerFromChannel(data.vid.v));
	});
	auto minimal = false;

	Notify::PeerUpdate update;
	using UpdateFlag = Notify::PeerUpdate::Flag;

	data.match([&](const MTPDchat &data) {
		const auto chat = result->asChat();

		const auto canAddMembers = chat->canAddMembers();
		if (chat->version() < data.vversion.v) {
			chat->setVersion(data.vversion.v);
			chat->invalidateParticipants();
		}

		chat->input = MTP_inputPeerChat(data.vid);
		chat->setName(qs(data.vtitle));
		chat->setPhoto(data.vphoto);
		chat->date = data.vdate.v;

		chat->setAdminRights(data.has_admin_rights()
			? data.vadmin_rights
			: MTPChatAdminRights(MTP_chatAdminRights(MTP_flags(0))));
		chat->setDefaultRestrictions(data.has_default_banned_rights()
			? data.vdefault_banned_rights
			: MTPChatBannedRights(
				MTP_chatBannedRights(MTP_flags(0), MTP_int(0))));

		const auto &migratedTo = data.has_migrated_to()
			? data.vmigrated_to
			: MTPInputChannel(MTP_inputChannelEmpty());
		migratedTo.match([&](const MTPDinputChannel &input) {
			const auto channel = this->channel(input.vchannel_id.v);
			channel->addFlags(MTPDchannel::Flag::f_megagroup);
			if (!channel->access) {
				channel->input = MTP_inputPeerChannel(
					input.vchannel_id,
					input.vaccess_hash);
				channel->inputChannel = migratedTo;
				channel->access = input.vaccess_hash.v;
			}
			ApplyMigration(chat, channel);
		}, [](const MTPDinputChannelEmpty &) {
		});

		chat->setFlags(data.vflags.v);
		chat->count = data.vparticipants_count.v;

		if (canAddMembers != chat->canAddMembers()) {
			update.flags |= UpdateFlag::RightsChanged;
		}
	}, [&](const MTPDchatForbidden &data) {
		const auto chat = result->asChat();

		const auto canAddMembers = chat->canAddMembers();

		chat->input = MTP_inputPeerChat(data.vid);
		chat->setName(qs(data.vtitle));
		chat->setPhoto(MTP_chatPhotoEmpty());
		chat->date = 0;
		chat->count = -1;
		chat->invalidateParticipants();
		chat->setFlags(MTPDchat_ClientFlag::f_forbidden | 0);
		chat->setAdminRights(MTP_chatAdminRights(MTP_flags(0)));
		chat->setDefaultRestrictions(
			MTP_chatBannedRights(MTP_flags(0), MTP_int(0)));

		if (canAddMembers != chat->canAddMembers()) {
			update.flags |= UpdateFlag::RightsChanged;
		}
	}, [&](const MTPDchannel &data) {
		const auto channel = result->asChannel();

		minimal = data.is_min();
		if (minimal) {
			if (result->loadedStatus != PeerData::FullLoaded) {
				LOG(("API Warning: not loaded minimal channel applied."));
			}
		} else {
			const auto accessHash = data.has_access_hash()
				? data.vaccess_hash
				: MTP_long(0);
			channel->input = MTP_inputPeerChannel(data.vid, accessHash);
		}

		const auto wasInChannel = channel->amIn();
		const auto canViewAdmins = channel->canViewAdmins();
		const auto canViewMembers = channel->canViewMembers();
		const auto canAddMembers = channel->canAddMembers();

		if (data.has_participants_count()) {
			channel->setMembersCount(data.vparticipants_count.v);
		}
		channel->setDefaultRestrictions(data.has_default_banned_rights()
			? data.vdefault_banned_rights
			: MTPChatBannedRights(
				MTP_chatBannedRights(MTP_flags(0), MTP_int(0))));
		if (minimal) {
			auto mask = 0
				| MTPDchannel::Flag::f_broadcast
				| MTPDchannel::Flag::f_verified
				| MTPDchannel::Flag::f_megagroup
				| MTPDchannel_ClientFlag::f_forbidden;
			channel->setFlags((channel->flags() & ~mask) | (data.vflags.v & mask));
		} else {
			if (data.has_admin_rights()) {
				channel->setAdminRights(data.vadmin_rights);
			} else if (channel->hasAdminRights()) {
				channel->setAdminRights(MTP_chatAdminRights(MTP_flags(0)));
			}
			if (data.has_banned_rights()) {
				channel->setRestrictions(data.vbanned_rights);
			} else if (channel->hasRestrictions()) {
				channel->setRestrictions(MTP_chatBannedRights(MTP_flags(0), MTP_int(0)));
			}
			channel->inputChannel = MTP_inputChannel(data.vid, data.vaccess_hash);
			channel->access = data.vaccess_hash.v;
			channel->date = data.vdate.v;
			if (channel->version() < data.vversion.v) {
				channel->setVersion(data.vversion.v);
			}
			channel->setUnavailableReason(data.is_restricted()
				? ExtractUnavailableReason(qs(data.vrestriction_reason))
				: QString());
			channel->setFlags(data.vflags.v);
			//if (data.has_feed_id()) { // #feed
			//	channel->setFeed(feed(data.vfeed_id.v));
			//} else {
			//	channel->clearFeed();
			//}
		}

		QString uname = data.has_username() ? TextUtilities::SingleLine(qs(data.vusername)) : QString();
		channel->setName(qs(data.vtitle), uname);

		channel->setPhoto(data.vphoto);

		if (wasInChannel != channel->amIn()) {
			update.flags |= UpdateFlag::ChannelAmIn;
		}
		if (canViewAdmins != channel->canViewAdmins()
			|| canViewMembers != channel->canViewMembers()
			|| canAddMembers != channel->canAddMembers()) {
			update.flags |= UpdateFlag::RightsChanged;
		}
	}, [&](const MTPDchannelForbidden &data) {
		const auto channel = result->asChannel();
		channel->input = MTP_inputPeerChannel(data.vid, data.vaccess_hash);

		auto wasInChannel = channel->amIn();
		auto canViewAdmins = channel->canViewAdmins();
		auto canViewMembers = channel->canViewMembers();
		auto canAddMembers = channel->canAddMembers();

		channel->inputChannel = MTP_inputChannel(data.vid, data.vaccess_hash);

		auto mask = mtpCastFlags(MTPDchannelForbidden::Flag::f_broadcast | MTPDchannelForbidden::Flag::f_megagroup);
		channel->setFlags((channel->flags() & ~mask) | (mtpCastFlags(data.vflags) & mask) | MTPDchannel_ClientFlag::f_forbidden);

		if (channel->hasAdminRights()) {
			channel->setAdminRights(MTP_chatAdminRights(MTP_flags(0)));
		}
		if (channel->hasRestrictions()) {
			channel->setRestrictions(MTP_chatBannedRights(MTP_flags(0), MTP_int(0)));
		}

		channel->setName(qs(data.vtitle), QString());

		channel->access = data.vaccess_hash.v;
		channel->setPhoto(MTP_chatPhotoEmpty());
		channel->date = 0;
		channel->setMembersCount(0);

		if (wasInChannel != channel->amIn()) {
			update.flags |= UpdateFlag::ChannelAmIn;
		}
		if (canViewAdmins != channel->canViewAdmins()
			|| canViewMembers != channel->canViewMembers()
			|| canAddMembers != channel->canAddMembers()) {
			update.flags |= UpdateFlag::RightsChanged;
		}
	}, [](const MTPDchatEmpty &) {
	});

	if (minimal) {
		if (result->loadedStatus == PeerData::NotLoaded) {
			result->loadedStatus = PeerData::MinimalLoaded;
		}
	} else if (result->loadedStatus != PeerData::FullLoaded) {
		result->loadedStatus = PeerData::FullLoaded;
	}
	if (update.flags) {
		update.peer = result;
		Notify::peerUpdatedDelayed(update);
	}
	return result;
}

UserData *Session::processUsers(const MTPVector<MTPUser> &data) {
	auto result = (UserData*)nullptr;
	for (const auto &user : data.v) {
		result = this->user(user);
	}
	return result;
}

PeerData *Session::processChats(const MTPVector<MTPChat> &data) {
	auto result = (PeerData*)nullptr;
	for (const auto &chat : data.v) {
		result = this->chat(chat);
	}
	return result;
}

void Session::applyMaximumChatVersions(const MTPVector<MTPChat> &data) {
	for (const auto &chat : data.v) {
		chat.match([&](const MTPDchat &data) {
			if (const auto chat = chatLoaded(data.vid.v)) {
				if (data.vversion.v < chat->version()) {
					chat->setVersion(data.vversion.v);
				}
			}
		}, [&](const MTPDchannel &data) {
			if (const auto channel = channelLoaded(data.vid.v)) {
				if (data.vversion.v < channel->version()) {
					channel->setVersion(data.vversion.v);
				}
			}
		}, [](const auto &) {
		});
	}
}

PeerData *Session::peerByUsername(const QString &username) const {
	const auto uname = username.trimmed();
	for (const auto &[peerId, peer] : _peers) {
		if (!peer->userName().compare(uname, Qt::CaseInsensitive)) {
			return peer.get();
		}
	}
	return nullptr;
}

void Session::enumerateUsers(Fn<void(not_null<UserData*>)> action) const {
	for (const auto &[peerId, peer] : _peers) {
		if (const auto user = peer->asUser()) {
			action(user);
		}
	}
}

void Session::enumerateGroups(Fn<void(not_null<PeerData*>)> action) const {
	for (const auto &[peerId, peer] : _peers) {
		if (peer->isChat() || peer->isMegagroup()) {
			action(peer.get());
		}
	}
}

void Session::enumerateChannels(
		Fn<void(not_null<ChannelData*>)> action) const {
	for (const auto &[peerId, peer] : _peers) {
		if (const auto channel = peer->asChannel()) {
			if (!channel->isMegagroup()) {
				action(channel);
			}
		}
	}
}

not_null<History*> Session::history(PeerId peerId) {
	Expects(peerId != 0);

	if (const auto result = historyLoaded(peerId)) {
		return result;
	}
	const auto [i, ok] = _histories.emplace(
		peerId,
		std::make_unique<History>(this, peerId));
	return i->second.get();
}

History *Session::historyLoaded(PeerId peerId) const {
	const auto i = peerId ? _histories.find(peerId) : end(_histories);
	return (i != end(_histories)) ? i->second.get() : nullptr;
}

not_null<History*> Session::history(not_null<const PeerData*> peer) {
	return history(peer->id);
}

History *Session::historyLoaded(const PeerData *peer) {
	return peer ? historyLoaded(peer->id) : nullptr;
}

void Session::registerSendAction(
		not_null<History*> history,
		not_null<UserData*> user,
		const MTPSendMessageAction &action,
		TimeId when) {
	if (history->updateSendActionNeedsAnimating(user, action)) {
		user->madeAction(when);

		const auto i = _sendActions.find(history);
		if (!_sendActions.contains(history)) {
			_sendActions.emplace(history, getms());
			_a_sendActions.start();
		}
	}
}

void Session::step_typings(TimeMs ms, bool timer) {
	for (auto i = begin(_sendActions); i != end(_sendActions);) {
		if (i->first->updateSendActionNeedsAnimating(ms)) {
			++i;
		} else {
			i = _sendActions.erase(i);
		}
	}
	if (_sendActions.empty()) {
		_a_sendActions.stop();
	}
}

Storage::Cache::Database &Session::cache() {
	return *_cache;
}

void Session::startExport(PeerData *peer) {
	startExport(peer ? peer->input : MTP_inputPeerEmpty());
}

void Session::startExport(const MTPInputPeer &singlePeer) {
	if (_exportPanel) {
		_exportPanel->activatePanel();
		return;
	}
	_export = std::make_unique<Export::ControllerWrap>(singlePeer);
	_exportPanel = std::make_unique<Export::View::PanelController>(
		_export.get());

	_exportViewChanges.fire(_exportPanel.get());

	_exportPanel->stopRequests(
	) | rpl::start_with_next([=] {
		LOG(("Export Info: Stop requested."));
		stopExport();
	}, _export->lifetime());
}

void Session::suggestStartExport(TimeId availableAt) {
	_exportAvailableAt = availableAt;
	suggestStartExport();
}

void Session::clearExportSuggestion() {
	_exportAvailableAt = 0;
	if (_exportSuggestion) {
		_exportSuggestion->closeBox();
	}
}

void Session::suggestStartExport() {
	if (_exportAvailableAt <= 0) {
		return;
	}

	const auto now = unixtime();
	const auto left = (_exportAvailableAt <= now)
		? 0
		: (_exportAvailableAt - now);
	if (left) {
		App::CallDelayed(
			std::min(left + 5, 3600) * TimeMs(1000),
			_session,
			[=] { suggestStartExport(); });
	} else if (_export) {
		Export::View::ClearSuggestStart();
	} else {
		_exportSuggestion = Export::View::SuggestStart();
	}
}

rpl::producer<Export::View::PanelController*> Session::currentExportView(
) const {
	return _exportViewChanges.events_starting_with(_exportPanel.get());
}

bool Session::exportInProgress() const {
	return _export != nullptr;
}

void Session::stopExportWithConfirmation(FnMut<void()> callback) {
	if (!_exportPanel) {
		callback();
		return;
	}
	auto closeAndCall = [=, callback = std::move(callback)]() mutable {
		auto saved = std::move(callback);
		LOG(("Export Info: Stop With Confirmation."));
		stopExport();
		if (saved) {
			saved();
		}
	};
	_exportPanel->stopWithConfirmation(std::move(closeAndCall));
}

void Session::stopExport() {
	if (_exportPanel) {
		LOG(("Export Info: Destroying."));
		_exportPanel = nullptr;
		_exportViewChanges.fire(nullptr);
	}
	_export = nullptr;
}

const Passport::SavedCredentials *Session::passportCredentials() const {
	return _passportCredentials ? &_passportCredentials->first : nullptr;
}

void Session::rememberPassportCredentials(
		Passport::SavedCredentials data,
		TimeMs rememberFor) {
	Expects(rememberFor > 0);

	static auto generation = 0;
	_passportCredentials = std::make_unique<CredentialsWithGeneration>(
		std::move(data),
		++generation);
	App::CallDelayed(rememberFor, _session, [=, check = generation] {
		if (_passportCredentials && _passportCredentials->second == check) {
			forgetPassportCredentials();
		}
	});
}

void Session::forgetPassportCredentials() {
	_passportCredentials = nullptr;
}

void Session::setupContactViewsViewer() {
	Notify::PeerUpdateViewer(
		Notify::PeerUpdate::Flag::UserIsContact
	) | rpl::map([](const Notify::PeerUpdate &update) {
		return update.peer->asUser();
	}) | rpl::filter([](UserData *user) {
		return user != nullptr;
	}) | rpl::start_with_next([=](not_null<UserData*> user) {
		userIsContactUpdated(user);
	}, _lifetime);
}

void Session::setupChannelLeavingViewer() {
	Notify::PeerUpdateViewer(
		Notify::PeerUpdate::Flag::ChannelAmIn
	) | rpl::map([](const Notify::PeerUpdate &update) {
		return update.peer->asChannel();
	}) | rpl::filter([](ChannelData *channel) {
		return (channel != nullptr)
			&& !(channel->amIn());
	}) | rpl::start_with_next([=](not_null<ChannelData*> channel) {
		channel->clearFeed();
		if (const auto history = historyLoaded(channel->id)) {
			history->removeJoinedMessage();
			history->updateChatListExistence();
			history->updateChatListSortPosition();
		}
	}, _lifetime);
}

Session::~Session() {
	// Optimization: clear notifications before destroying items.
	_session->notifications().clearAllFast();

	clear();
	Images::ClearRemote();
}

template <typename Method>
void Session::enumerateItemViews(
		not_null<const HistoryItem*> item,
		Method method) {
	if (const auto i = _views.find(item); i != _views.end()) {
		for (const auto view : i->second) {
			method(view);
		}
	}
}

void Session::photoLoadSettingsChanged() {
	for (const auto &[id, photo] : _photos) {
		photo->automaticLoadSettingsChanged();
	}
}

void Session::documentLoadSettingsChanged() {
	for (const auto &[id, document] : _documents) {
		document->automaticLoadSettingsChanged();
	}
}

void Session::notifyPhotoLayoutChanged(not_null<const PhotoData*> photo) {
	if (const auto i = _photoItems.find(photo); i != end(_photoItems)) {
		for (const auto item : i->second) {
			notifyItemLayoutChange(item);
		}
	}
}

void Session::notifyDocumentLayoutChanged(
		not_null<const DocumentData*> document) {
	const auto i = _documentItems.find(document);
	if (i != end(_documentItems)) {
		for (const auto item : i->second) {
			notifyItemLayoutChange(item);
		}
	}
	if (const auto items = InlineBots::Layout::documentItems()) {
		if (const auto i = items->find(document); i != items->end()) {
			for (const auto item : i->second) {
				item->layoutChanged();
			}
		}
	}
}

void Session::requestDocumentViewRepaint(
		not_null<const DocumentData*> document) {
	const auto i = _documentItems.find(document);
	if (i != end(_documentItems)) {
		for (const auto item : i->second) {
			requestItemRepaint(item);
		}
	}
}

void Session::requestPollViewRepaint(not_null<const PollData*> poll) {
	if (const auto i = _pollViews.find(poll); i != _pollViews.end()) {
		for (const auto view : i->second) {
			requestViewResize(view);
		}
	}
}

void Session::markMediaRead(not_null<const DocumentData*> document) {
	const auto i = _documentItems.find(document);
	if (i != end(_documentItems)) {
		_session->api().markMediaRead({ begin(i->second), end(i->second) });
	}
}

void Session::notifyItemLayoutChange(not_null<const HistoryItem*> item) {
	_itemLayoutChanges.fire_copy(item);
	enumerateItemViews(item, [&](not_null<ViewElement*> view) {
		notifyViewLayoutChange(view);
	});
}

rpl::producer<not_null<const HistoryItem*>> Session::itemLayoutChanged() const {
	return _itemLayoutChanges.events();
}

void Session::notifyViewLayoutChange(not_null<const ViewElement*> view) {
	_viewLayoutChanges.fire_copy(view);
}

rpl::producer<not_null<const ViewElement*>> Session::viewLayoutChanged() const {
	return _viewLayoutChanges.events();
}

void Session::notifyItemIdChange(IdChange event) {
	_itemIdChanges.fire_copy(event);

	const auto refreshViewDataId = [](not_null<ViewElement*> view) {
		view->refreshDataId();
	};
	enumerateItemViews(event.item, refreshViewDataId);
	if (const auto group = groups().find(event.item)) {
		const auto leader = group->items.back();
		if (leader != event.item) {
			enumerateItemViews(leader, refreshViewDataId);
		}
	}
}

rpl::producer<Session::IdChange> Session::itemIdChanged() const {
	return _itemIdChanges.events();
}

void Session::requestItemRepaint(not_null<const HistoryItem*> item) {
	_itemRepaintRequest.fire_copy(item);
	enumerateItemViews(item, [&](not_null<const ViewElement*> view) {
		requestViewRepaint(view);
	});
}

rpl::producer<not_null<const HistoryItem*>> Session::itemRepaintRequest() const {
	return _itemRepaintRequest.events();
}

void Session::requestViewRepaint(not_null<const ViewElement*> view) {
	_viewRepaintRequest.fire_copy(view);
}

rpl::producer<not_null<const ViewElement*>> Session::viewRepaintRequest() const {
	return _viewRepaintRequest.events();
}

void Session::requestItemResize(not_null<const HistoryItem*> item) {
	_itemResizeRequest.fire_copy(item);
	enumerateItemViews(item, [&](not_null<ViewElement*> view) {
		requestViewResize(view);
	});
}

rpl::producer<not_null<const HistoryItem*>> Session::itemResizeRequest() const {
	return _itemResizeRequest.events();
}

void Session::requestViewResize(not_null<ViewElement*> view) {
	view->setPendingResize();
	_viewResizeRequest.fire_copy(view);
	notifyViewLayoutChange(view);
}

rpl::producer<not_null<ViewElement*>> Session::viewResizeRequest() const {
	return _viewResizeRequest.events();
}

void Session::requestItemViewRefresh(not_null<HistoryItem*> item) {
	if (const auto view = item->mainView()) {
		view->setPendingResize();
	}
	_itemViewRefreshRequest.fire_copy(item);
}

rpl::producer<not_null<HistoryItem*>> Session::itemViewRefreshRequest() const {
	return _itemViewRefreshRequest.events();
}

void Session::requestItemTextRefresh(not_null<HistoryItem*> item) {
	if (const auto i = _views.find(item); i != _views.end()) {
		for (const auto view : i->second) {
			if (const auto media = view->media()) {
				media->parentTextUpdated();
			}
		}
	}
}

void Session::requestAnimationPlayInline(not_null<HistoryItem*> item) {
	_animationPlayInlineRequest.fire_copy(item);
}

rpl::producer<not_null<HistoryItem*>> Session::animationPlayInlineRequest() const {
	return _animationPlayInlineRequest.events();
}

void Session::notifyItemRemoved(not_null<const HistoryItem*> item) {
	_itemRemoved.fire_copy(item);
	groups().unregisterMessage(item);
}

rpl::producer<not_null<const HistoryItem*>> Session::itemRemoved() const {
	return _itemRemoved.events();
}

void Session::notifyViewRemoved(not_null<const ViewElement*> view) {
	_viewRemoved.fire_copy(view);
}

rpl::producer<not_null<const ViewElement*>> Session::viewRemoved() const {
	return _viewRemoved.events();
}

void Session::notifyHistoryUnloaded(not_null<const History*> history) {
	_historyUnloaded.fire_copy(history);
}

rpl::producer<not_null<const History*>> Session::historyUnloaded() const {
	return _historyUnloaded.events();
}

void Session::notifyHistoryCleared(not_null<const History*> history) {
	_historyCleared.fire_copy(history);
}

rpl::producer<not_null<const History*>> Session::historyCleared() const {
	return _historyCleared.events();
}

void Session::notifyHistoryChangeDelayed(not_null<History*> history) {
	history->setHasPendingResizedItems();
	_historiesChanged.insert(history);
}

rpl::producer<not_null<History*>> Session::historyChanged() const {
	return _historyChanged.events();
}

void Session::sendHistoryChangeNotifications() {
	for (const auto history : base::take(_historiesChanged)) {
		_historyChanged.fire_copy(history);
	}
}

void Session::removeMegagroupParticipant(
		not_null<ChannelData*> channel,
		not_null<UserData*> user) {
	_megagroupParticipantRemoved.fire({ channel, user });
}

auto Session::megagroupParticipantRemoved() const
-> rpl::producer<MegagroupParticipant> {
	return _megagroupParticipantRemoved.events();
}

rpl::producer<not_null<UserData*>> Session::megagroupParticipantRemoved(
		not_null<ChannelData*> channel) const {
	return megagroupParticipantRemoved(
	) | rpl::filter([channel](auto updateChannel, auto user) {
		return (updateChannel == channel);
	}) | rpl::map([](auto updateChannel, auto user) {
		return user;
	});
}

void Session::addNewMegagroupParticipant(
		not_null<ChannelData*> channel,
		not_null<UserData*> user) {
	_megagroupParticipantAdded.fire({ channel, user });
}

auto Session::megagroupParticipantAdded() const
-> rpl::producer<MegagroupParticipant> {
	return _megagroupParticipantAdded.events();
}

rpl::producer<not_null<UserData*>> Session::megagroupParticipantAdded(
		not_null<ChannelData*> channel) const {
	return megagroupParticipantAdded(
	) | rpl::filter([channel](auto updateChannel, auto user) {
		return (updateChannel == channel);
	}) | rpl::map([](auto updateChannel, auto user) {
		return user;
	});
}

void Session::notifyFeedUpdated(
		not_null<Feed*> feed,
		FeedUpdateFlag update) {
	_feedUpdates.fire({ feed, update });
}

rpl::producer<FeedUpdate> Session::feedUpdated() const {
	return _feedUpdates.events();
}

void Session::notifyStickersUpdated() {
	_stickersUpdated.fire({});
}

rpl::producer<> Session::stickersUpdated() const {
	return _stickersUpdated.events();
}

void Session::notifySavedGifsUpdated() {
	_savedGifsUpdated.fire({});
}

rpl::producer<> Session::savedGifsUpdated() const {
	return _savedGifsUpdated.events();
}

void Session::userIsContactUpdated(not_null<UserData*> user) {
	const auto i = _contactViews.find(peerToUser(user->id));
	if (i != _contactViews.end()) {
		for (const auto view : i->second) {
			requestViewResize(view);
		}
	}
}

HistoryItemsList Session::idsToItems(
		const MessageIdsList &ids) const {
	return ranges::view::all(
		ids
	) | ranges::view::transform([](const FullMsgId &fullId) {
		return App::histItemById(fullId);
	}) | ranges::view::filter([](HistoryItem *item) {
		return item != nullptr;
	}) | ranges::view::transform([](HistoryItem *item) {
		return not_null<HistoryItem*>(item);
	}) | ranges::to_vector;
}

MessageIdsList Session::itemsToIds(
		const HistoryItemsList &items) const {
	return ranges::view::all(
		items
	) | ranges::view::transform([](not_null<HistoryItem*> item) {
		return item->fullId();
	}) | ranges::to_vector;
}

MessageIdsList Session::itemOrItsGroup(not_null<HistoryItem*> item) const {
	if (const auto group = groups().find(item)) {
		return itemsToIds(group->items);
	}
	return { 1, item->fullId() };
}

void Session::setPinnedDialog(const Dialogs::Key &key, bool pinned) {
	setIsPinned(key, pinned);
}

void Session::applyPinnedDialogs(const QVector<MTPDialog> &list) {
	clearPinnedDialogs();
	for (auto i = list.size(); i != 0;) {
		const auto &dialog = list[--i];
		switch (dialog.type()) {
		case mtpc_dialog: {
			const auto &dialogData = dialog.c_dialog();
			if (const auto peer = peerFromMTP(dialogData.vpeer)) {
				setPinnedDialog(history(peer), true);
			}
		} break;

		//case mtpc_dialogFeed: { // #feed
		//	const auto &feedData = dialog.c_dialogFeed();
		//	const auto feedId = feedData.vfeed_id.v;
		//	setPinnedDialog(feed(feedId), true);
		//} break;

		default: Unexpected("Type in ApiWrap::applyDialogsPinned.");
		}
	}
}

void Session::applyPinnedDialogs(const QVector<MTPDialogPeer> &list) {
	clearPinnedDialogs();
	for (auto i = list.size(); i != 0;) {
		const auto &dialogPeer = list[--i];
		switch (dialogPeer.type()) {
		case mtpc_dialogPeer: {
			const auto &peerData = dialogPeer.c_dialogPeer();
			if (const auto peerId = peerFromMTP(peerData.vpeer)) {
				setPinnedDialog(history(peerId), true);
			}
		} break;
		//case mtpc_dialogPeerFeed: { // #feed
		//	const auto &feedData = dialogPeer.c_dialogPeerFeed();
		//	const auto feedId = feedData.vfeed_id.v;
		//	setPinnedDialog(feed(feedId), true);
		//} break;
		}
	}
}

int Session::pinnedDialogsCount() const {
	return _pinnedDialogs.size();
}

const std::deque<Dialogs::Key> &Session::pinnedDialogsOrder() const {
	return _pinnedDialogs;
}

void Session::clearPinnedDialogs() {
	while (!_pinnedDialogs.empty()) {
		setPinnedDialog(_pinnedDialogs.back(), false);
	}
}

void Session::reorderTwoPinnedDialogs(
		const Dialogs::Key &key1,
		const Dialogs::Key &key2) {
	const auto &order = pinnedDialogsOrder();
	const auto index1 = ranges::find(order, key1) - begin(order);
	const auto index2 = ranges::find(order, key2) - begin(order);
	Assert(index1 >= 0 && index1 < order.size());
	Assert(index2 >= 0 && index2 < order.size());
	Assert(index1 != index2);
	std::swap(_pinnedDialogs[index1], _pinnedDialogs[index2]);
	key1.entry()->cachePinnedIndex(index2 + 1);
	key2.entry()->cachePinnedIndex(index1 + 1);
}

void Session::setIsPinned(const Dialogs::Key &key, bool pinned) {
	const auto already = ranges::find(_pinnedDialogs, key);
	if (pinned) {
		if (already != end(_pinnedDialogs)) {
			auto saved = std::move(*already);
			const auto alreadyIndex = already - end(_pinnedDialogs);
			const auto count = int(size(_pinnedDialogs));
			Assert(alreadyIndex < count);
			for (auto index = alreadyIndex + 1; index != count; ++index) {
				_pinnedDialogs[index - 1] = std::move(_pinnedDialogs[index]);
				_pinnedDialogs[index - 1].entry()->cachePinnedIndex(index);
			}
			_pinnedDialogs.back() = std::move(saved);
			_pinnedDialogs.back().entry()->cachePinnedIndex(count);
		} else {
			_pinnedDialogs.push_back(key);
			if (_pinnedDialogs.size() > Global::PinnedDialogsCountMax()) {
				_pinnedDialogs.front().entry()->cachePinnedIndex(0);
				_pinnedDialogs.pop_front();

				auto index = 0;
				for (const auto &pinned : _pinnedDialogs) {
					pinned.entry()->cachePinnedIndex(++index);
				}
			} else {
				key.entry()->cachePinnedIndex(_pinnedDialogs.size());
			}
		}
	} else if (!pinned && already != end(_pinnedDialogs)) {
		key.entry()->cachePinnedIndex(0);
		_pinnedDialogs.erase(already);
		auto index = 0;
		for (const auto &pinned : _pinnedDialogs) {
			pinned.entry()->cachePinnedIndex(++index);
		}
	}
}

NotifySettings &Session::defaultNotifySettings(
		not_null<const PeerData*> peer) {
	return peer->isUser()
		? _defaultUserNotifySettings
		: _defaultChatNotifySettings;
}

const NotifySettings &Session::defaultNotifySettings(
		not_null<const PeerData*> peer) const {
	return peer->isUser()
		? _defaultUserNotifySettings
		: _defaultChatNotifySettings;
}

void Session::updateNotifySettingsLocal(not_null<PeerData*> peer) {
	const auto history = historyLoaded(peer->id);
	auto changesIn = TimeMs(0);
	const auto muted = notifyIsMuted(peer, &changesIn);
	if (history && history->changeMute(muted)) {
		// Notification already sent.
	} else {
		Notify::peerUpdatedDelayed(
			peer,
			Notify::PeerUpdate::Flag::NotificationsEnabled);
	}

	if (muted) {
		_mutedPeers.emplace(peer);
		unmuteByFinishedDelayed(changesIn);
		if (history) {
			_session->notifications().clearFromHistory(history);
		}
	} else {
		_mutedPeers.erase(peer);
	}
}

void Session::unmuteByFinishedDelayed(TimeMs delay) {
	accumulate_min(delay, kMaxNotifyCheckDelay);
	if (!_unmuteByFinishedTimer.isActive()
		|| _unmuteByFinishedTimer.remainingTime() > delay) {
		_unmuteByFinishedTimer.callOnce(delay);
	}
}

void Session::unmuteByFinished() {
	auto changesInMin = TimeMs(0);
	for (auto i = begin(_mutedPeers); i != end(_mutedPeers);) {
		const auto history = historyLoaded((*i)->id);
		auto changesIn = TimeMs(0);
		const auto muted = notifyIsMuted(*i, &changesIn);
		if (muted) {
			if (history) {
				history->changeMute(true);
			}
			if (!changesInMin || changesInMin > changesIn) {
				changesInMin = changesIn;
			}
			++i;
		} else {
			if (history) {
				history->changeMute(false);
			}
			i = _mutedPeers.erase(i);
		}
	}
	if (changesInMin) {
		unmuteByFinishedDelayed(changesInMin);
	}
}

HistoryItem *Session::addNewMessage(
		const MTPMessage &data,
		NewMessageType type) {
	const auto peerId = PeerFromMessage(data);
	if (!peerId) {
		return nullptr;
	}

	const auto result = history(peerId)->addNewMessage(data, type);
	if (result && type == NewMessageUnread) {
		CheckForSwitchInlineButton(result);
	}
	return result;
}

auto Session::sendActionAnimationUpdated() const
-> rpl::producer<SendActionAnimationUpdate> {
	return _sendActionAnimationUpdate.events();
}

void Session::updateSendActionAnimation(
		SendActionAnimationUpdate &&update) {
	_sendActionAnimationUpdate.fire(std::move(update));
}

int Session::unreadBadge() const {
	return computeUnreadBadge(
		_unreadFull,
		_unreadMuted,
		_unreadEntriesFull,
		_unreadEntriesMuted);
}

bool Session::unreadBadgeMuted() const {
	return computeUnreadBadgeMuted(
		_unreadFull,
		_unreadMuted,
		_unreadEntriesFull,
		_unreadEntriesMuted);
}

int Session::unreadBadgeIgnoreOne(History *history) const {
	const auto removeCount = (history
		&& history->inChatList(Dialogs::Mode::All))
		? history->unreadCount()
		: 0;
	if (!removeCount) {
		return unreadBadge();
	}
	const auto removeMuted = history->mute();
	return computeUnreadBadge(
		_unreadFull - removeCount,
		_unreadMuted - (removeMuted ? removeCount : 0),
		_unreadEntriesFull - 1,
		_unreadEntriesMuted - (removeMuted ? 1 : 0));
}

bool Session::unreadBadgeMutedIgnoreOne(History *history) const {
	const auto removeCount = (history
		&& history->inChatList(Dialogs::Mode::All))
		? history->unreadCount()
		: 0;
	if (!removeCount) {
		return unreadBadgeMuted();
	}
	const auto removeMuted = history->mute();
	return computeUnreadBadgeMuted(
		_unreadFull - removeCount,
		_unreadMuted - (removeMuted ? removeCount : 0),
		_unreadEntriesFull - 1,
		_unreadEntriesMuted - (removeMuted ? 1 : 0));
}

int Session::unreadOnlyMutedBadge() const {
	return _session->settings().countUnreadMessages()
		? _unreadMuted
		: _unreadEntriesMuted;
}

int Session::computeUnreadBadge(
		int full,
		int muted,
		int entriesFull,
		int entriesMuted) const {
	const auto withMuted = _session->settings().includeMutedCounter();
	return _session->settings().countUnreadMessages()
		? (full - (withMuted ? 0 : muted))
		: (entriesFull - (withMuted ? 0 : entriesMuted));
}

bool Session::computeUnreadBadgeMuted(
		int full,
		int muted,
		int entriesFull,
		int entriesMuted) const {
	if (!_session->settings().includeMutedCounter()) {
		return false;
	}
	return _session->settings().countUnreadMessages()
		? (muted >= full)
		: (entriesMuted >= entriesFull);
}

void Session::unreadIncrement(int count, bool muted) {
	if (!count) {
		return;
	}
	_unreadFull += count;
	if (muted) {
		_unreadMuted += count;
	}
	if (_session->settings().countUnreadMessages()) {
		if (!muted || _session->settings().includeMutedCounter()) {
			Notify::unreadCounterUpdated();
		}
	}
}

void Session::unreadMuteChanged(int count, bool muted) {
	const auto wasAll = (_unreadMuted == _unreadFull);
	if (muted) {
		_unreadMuted += count;
	} else {
		_unreadMuted -= count;
	}
	if (_session->settings().countUnreadMessages()) {
		const auto nowAll = (_unreadMuted == _unreadFull);
		const auto changed = !_session->settings().includeMutedCounter()
			|| (wasAll != nowAll);
		if (changed) {
			Notify::unreadCounterUpdated();
		}
	}
}

void Session::unreadEntriesChanged(
		int withUnreadDelta,
		int mutedWithUnreadDelta) {
	if (!withUnreadDelta && !mutedWithUnreadDelta) {
		return;
	}
	const auto wasAll = (_unreadEntriesMuted == _unreadEntriesFull);
	_unreadEntriesFull += withUnreadDelta;
	_unreadEntriesMuted += mutedWithUnreadDelta;
	if (!_session->settings().countUnreadMessages()) {
		const auto nowAll = (_unreadEntriesMuted == _unreadEntriesFull);
		const auto withMuted = _session->settings().includeMutedCounter();
		const auto withMutedChanged = withMuted
			&& (withUnreadDelta != 0 || wasAll != nowAll);
		const auto withoutMutedChanged = !withMuted
			&& (withUnreadDelta != mutedWithUnreadDelta);
		if (withMutedChanged || withoutMutedChanged) {
			Notify::unreadCounterUpdated();
		}
	}
}

void Session::selfDestructIn(not_null<HistoryItem*> item, TimeMs delay) {
	_selfDestructItems.push_back(item->fullId());
	if (!_selfDestructTimer.isActive()
		|| _selfDestructTimer.remainingTime() > delay) {
		_selfDestructTimer.callOnce(delay);
	}
}

void Session::checkSelfDestructItems() {
	auto now = getms(true);
	auto nextDestructIn = TimeMs(0);
	for (auto i = _selfDestructItems.begin(); i != _selfDestructItems.cend();) {
		if (auto item = App::histItemById(*i)) {
			if (auto destructIn = item->getSelfDestructIn(now)) {
				if (nextDestructIn > 0) {
					accumulate_min(nextDestructIn, destructIn);
				} else {
					nextDestructIn = destructIn;
				}
				++i;
			} else {
				i = _selfDestructItems.erase(i);
			}
		} else {
			i = _selfDestructItems.erase(i);
		}
	}
	if (nextDestructIn > 0) {
		_selfDestructTimer.callOnce(nextDestructIn);
	}
}

not_null<PhotoData*> Session::photo(PhotoId id) {
	auto i = _photos.find(id);
	if (i == _photos.end()) {
		i = _photos.emplace(id, std::make_unique<PhotoData>(id)).first;
	}
	return i->second.get();
}

not_null<PhotoData*> Session::photo(const MTPPhoto &data) {
	switch (data.type()) {
	case mtpc_photo:
		return photo(data.c_photo());

	case mtpc_photoEmpty:
		return photo(data.c_photoEmpty().vid.v);
	}
	Unexpected("Type in Session::photo().");
}

not_null<PhotoData*> Session::photo(const MTPDphoto &data) {
	const auto result = photo(data.vid.v);
	photoApplyFields(result, data);
	return result;
}

not_null<PhotoData*> Session::photo(
		const MTPPhoto &data,
		const PreparedPhotoThumbs &thumbs) {
	auto thumb = (const QImage*)nullptr;
	auto medium = (const QImage*)nullptr;
	auto full = (const QImage*)nullptr;
	auto thumbLevel = -1;
	auto mediumLevel = -1;
	auto fullLevel = -1;
	for (auto i = thumbs.cbegin(), e = thumbs.cend(); i != e; ++i) {
		const auto newThumbLevel = ThumbLevels.indexOf(i.key());
		const auto newMediumLevel = MediumLevels.indexOf(i.key());
		const auto newFullLevel = FullLevels.indexOf(i.key());
		if (newThumbLevel < 0 || newMediumLevel < 0 || newFullLevel < 0) {
			continue;
		}
		if (thumbLevel < 0 || newThumbLevel < thumbLevel) {
			thumbLevel = newThumbLevel;
			thumb = &i.value();
		}
		if (mediumLevel < 0 || newMediumLevel < mediumLevel) {
			mediumLevel = newMediumLevel;
			medium = &i.value();
		}
		if (fullLevel < 0 || newFullLevel < fullLevel) {
			fullLevel = newFullLevel;
			full = &i.value();
		}
	}
	if (!thumb || !medium || !full) {
		return photo(0);
	}
	switch (data.type()) {
	case mtpc_photo:
		return photo(
			data.c_photo().vid.v,
			data.c_photo().vaccess_hash.v,
			data.c_photo().vfile_reference.v,
			data.c_photo().vdate.v,
			Images::Create(base::duplicate(*thumb), "JPG"),
			Images::Create(base::duplicate(*medium), "JPG"),
			Images::Create(base::duplicate(*full), "JPG"));

	case mtpc_photoEmpty:
		return photo(data.c_photoEmpty().vid.v);
	}
	Unexpected("Type in Session::photo() with prepared thumbs.");
}

not_null<PhotoData*> Session::photo(
		PhotoId id,
		const uint64 &access,
		const QByteArray &fileReference,
		TimeId date,
		const ImagePtr &thumb,
		const ImagePtr &medium,
		const ImagePtr &full) {
	const auto result = photo(id);
	photoApplyFields(
		result,
		access,
		fileReference,
		date,
		thumb,
		medium,
		full);
	return result;
}

void Session::photoConvert(
		not_null<PhotoData*> original,
		const MTPPhoto &data) {
	const auto id = [&] {
		switch (data.type()) {
		case mtpc_photo: return data.c_photo().vid.v;
		case mtpc_photoEmpty: return data.c_photoEmpty().vid.v;
		}
		Unexpected("Type in Session::photoConvert().");
	}();
	if (original->id != id) {
		auto i = _photos.find(id);
		if (i == _photos.end()) {
			const auto j = _photos.find(original->id);
			Assert(j != _photos.end());
			auto owned = std::move(j->second);
			_photos.erase(j);
			i = _photos.emplace(id, std::move(owned)).first;
		}

		original->id = id;
		original->uploadingData = nullptr;

		if (i->second.get() != original) {
			photoApplyFields(i->second.get(), data);
		}
	}
	photoApplyFields(original, data);
}

PhotoData *Session::photoFromWeb(
		const MTPWebDocument &data,
		ImagePtr thumb,
		bool willBecomeNormal) {
	const auto full = Images::Create(data);
	if (full->isNull()) {
		return nullptr;
	}
	auto medium = ImagePtr();
	if (willBecomeNormal) {
		const auto width = full->width();
		const auto height = full->height();
		if (thumb->isNull()) {
			auto thumbsize = shrinkToKeepAspect(width, height, 100, 100);
			thumb = Images::Create(thumbsize.width(), thumbsize.height());
		}

		auto mediumsize = shrinkToKeepAspect(width, height, 320, 320);
		medium = Images::Create(mediumsize.width(), mediumsize.height());
	}

	return photo(
		rand_value<PhotoId>(),
		uint64(0),
		QByteArray(),
		unixtime(),
		thumb,
		medium,
		full);
}

void Session::photoApplyFields(
		not_null<PhotoData*> photo,
		const MTPPhoto &data) {
	if (data.type() == mtpc_photo) {
		photoApplyFields(photo, data.c_photo());
	}
}

void Session::photoApplyFields(
		not_null<PhotoData*> photo,
		const MTPDphoto &data) {
	auto thumb = (const MTPPhotoSize*)nullptr;
	auto medium = (const MTPPhotoSize*)nullptr;
	auto full = (const MTPPhotoSize*)nullptr;
	auto thumbLevel = -1;
	auto mediumLevel = -1;
	auto fullLevel = -1;
	for (const auto &sizeData : data.vsizes.v) {
		const auto sizeLetter = sizeData.match([](MTPDphotoSizeEmpty) {
			return char(0);
		}, [](const auto &size) {
			return size.vtype.v.isEmpty() ? char(0) : size.vtype.v[0];
		});
		if (!sizeLetter) continue;

		const auto newThumbLevel = ThumbLevels.indexOf(sizeLetter);
		const auto newMediumLevel = MediumLevels.indexOf(sizeLetter);
		const auto newFullLevel = FullLevels.indexOf(sizeLetter);
		if (newThumbLevel < 0 || newMediumLevel < 0 || newFullLevel < 0) {
			continue;
		}
		if (thumbLevel < 0 || newThumbLevel < thumbLevel) {
			thumbLevel = newThumbLevel;
			thumb = &sizeData;
		}
		if (mediumLevel < 0 || newMediumLevel < mediumLevel) {
			mediumLevel = newMediumLevel;
			medium = &sizeData;
		}
		if (fullLevel < 0 || newFullLevel < fullLevel) {
			fullLevel = newFullLevel;
			full = &sizeData;
		}
	}
	if (thumb && medium && full) {
		photoApplyFields(
			photo,
			data.vaccess_hash.v,
			data.vfile_reference.v,
			data.vdate.v,
			App::image(*thumb),
			App::image(*medium),
			App::image(*full));
	}
}

void Session::photoApplyFields(
		not_null<PhotoData*> photo,
		const uint64 &access,
		const QByteArray &fileReference,
		TimeId date,
		const ImagePtr &thumb,
		const ImagePtr &medium,
		const ImagePtr &full) {
	if (!date) {
		return;
	}
	photo->access = access;
	photo->fileReference = fileReference;
	photo->date = date;
	UpdateImage(photo->thumb, thumb);
	UpdateImage(photo->medium, medium);
	UpdateImage(photo->full, full);
}

not_null<DocumentData*> Session::document(DocumentId id) {
	auto i = _documents.find(id);
	if (i == _documents.cend()) {
		i = _documents.emplace(
			id,
			std::make_unique<DocumentData>(id, _session)).first;
	}
	return i->second.get();
}

not_null<DocumentData*> Session::document(const MTPDocument &data) {
	switch (data.type()) {
	case mtpc_document:
		return document(data.c_document());

	case mtpc_documentEmpty:
		return document(data.c_documentEmpty().vid.v);
	}
	Unexpected("Type in Session::document().");
}

not_null<DocumentData*> Session::document(const MTPDdocument &data) {
	const auto result = document(data.vid.v);
	documentApplyFields(result, data);
	return result;
}

not_null<DocumentData*> Session::document(
		const MTPdocument &data,
		QImage &&thumb) {
	switch (data.type()) {
	case mtpc_documentEmpty:
		return document(data.c_documentEmpty().vid.v);

	case mtpc_document: {
		const auto &fields = data.c_document();
		return document(
			fields.vid.v,
			fields.vaccess_hash.v,
			fields.vfile_reference.v,
			fields.vdate.v,
			fields.vattributes.v,
			qs(fields.vmime_type),
			Images::Create(std::move(thumb), "JPG"),
			fields.vdc_id.v,
			fields.vsize.v,
			StorageImageLocation());
	} break;
	}
	Unexpected("Type in Session::document() with thumb.");
}

not_null<DocumentData*> Session::document(
		DocumentId id,
		const uint64 &access,
		const QByteArray &fileReference,
		TimeId date,
		const QVector<MTPDocumentAttribute> &attributes,
		const QString &mime,
		const ImagePtr &thumb,
		int32 dc,
		int32 size,
		const StorageImageLocation &thumbLocation) {
	const auto result = document(id);
	documentApplyFields(
		result,
		access,
		fileReference,
		date,
		attributes,
		mime,
		thumb,
		dc,
		size,
		thumbLocation);
	return result;
}

void Session::documentConvert(
		not_null<DocumentData*> original,
		const MTPDocument &data) {
	const auto id = [&] {
		switch (data.type()) {
		case mtpc_document: return data.c_document().vid.v;
		case mtpc_documentEmpty: return data.c_documentEmpty().vid.v;
		}
		Unexpected("Type in Session::documentConvert().");
	}();
	const auto oldKey = original->mediaKey();
	const auto oldCacheKey = original->cacheKey();
	const auto idChanged = (original->id != id);
	const auto sentSticker = idChanged && (original->sticker() != nullptr);
	if (idChanged) {
		auto i = _documents.find(id);
		if (i == _documents.end()) {
			const auto j = _documents.find(original->id);
			Assert(j != _documents.end());
			auto owned = std::move(j->second);
			_documents.erase(j);
			i = _documents.emplace(id, std::move(owned)).first;
		}

		original->id = id;
		original->status = FileReady;
		original->uploadingData = nullptr;

		if (i->second.get() != original) {
			documentApplyFields(i->second.get(), data);
		}
	}
	documentApplyFields(original, data);
	if (idChanged) {
		cache().moveIfEmpty(oldCacheKey, original->cacheKey());
		if (savedGifs().indexOf(original) >= 0) {
			Local::writeSavedGifs();
		}
	}
}

DocumentData *Session::documentFromWeb(
		const MTPWebDocument &data,
		ImagePtr thumb) {
	switch (data.type()) {
	case mtpc_webDocument:
		return documentFromWeb(data.c_webDocument(), thumb);

	case mtpc_webDocumentNoProxy:
		return documentFromWeb(data.c_webDocumentNoProxy(), thumb);

	}
	Unexpected("Type in Session::documentFromWeb.");
}

DocumentData *Session::documentFromWeb(
		const MTPDwebDocument &data,
		ImagePtr thumb) {
	const auto result = document(
		rand_value<DocumentId>(),
		uint64(0),
		QByteArray(),
		unixtime(),
		data.vattributes.v,
		data.vmime_type.v,
		thumb,
		MTP::maindc(),
		int32(0), // data.vsize.v
		StorageImageLocation());
	result->setWebLocation(WebFileLocation(
		Global::WebFileDcId(),
		data.vurl.v,
		data.vaccess_hash.v));
	return result;
}

DocumentData *Session::documentFromWeb(
		const MTPDwebDocumentNoProxy &data,
		ImagePtr thumb) {
	const auto result = document(
		rand_value<DocumentId>(),
		uint64(0),
		QByteArray(),
		unixtime(),
		data.vattributes.v,
		data.vmime_type.v,
		thumb,
		MTP::maindc(),
		int32(0), // data.vsize.v
		StorageImageLocation());
	result->setContentUrl(qs(data.vurl));
	return result;
}

void Session::documentApplyFields(
		not_null<DocumentData*> document,
		const MTPDocument &data) {
	if (data.type() == mtpc_document) {
		documentApplyFields(document, data.c_document());
	}
}

void Session::documentApplyFields(
		not_null<DocumentData*> document,
		const MTPDdocument &data) {
	const auto thumb = FindDocumentThumb(data);
	documentApplyFields(
		document,
		data.vaccess_hash.v,
		data.vfile_reference.v,
		data.vdate.v,
		data.vattributes.v,
		qs(data.vmime_type),
		App::image(thumb),
		data.vdc_id.v,
		data.vsize.v,
		StorageImageLocation::FromMTP(thumb));
}

void Session::documentApplyFields(
		not_null<DocumentData*> document,
		const uint64 &access,
		const QByteArray &fileReference,
		TimeId date,
		const QVector<MTPDocumentAttribute> &attributes,
		const QString &mime,
		const ImagePtr &thumb,
		int32 dc,
		int32 size,
		const StorageImageLocation &thumbLocation) {
	if (!date) {
		return;
	}
	document->setattributes(attributes);
	if (dc != 0 && access != 0) {
		document->setRemoteLocation(dc, access, fileReference);
	}
	document->date = date;
	document->setMimeString(mime);
	if (!thumb->isNull()
		&& (document->thumb->isNull()
			|| (document->sticker()
				&& (document->thumb->width() < thumb->width()
					|| document->thumb->height() < thumb->height())))) {
		document->thumb = thumb;
	}
	document->size = size;
	document->recountIsImage();
	if (document->sticker()
		&& document->sticker()->loc.isNull()
		&& !thumbLocation.isNull()) {
		document->sticker()->loc = thumbLocation;
	}
}

not_null<WebPageData*> Session::webpage(WebPageId id) {
	auto i = _webpages.find(id);
	if (i == _webpages.cend()) {
		i = _webpages.emplace(id, std::make_unique<WebPageData>(id)).first;
	}
	return i->second.get();
}

not_null<WebPageData*> Session::webpage(const MTPWebPage &data) {
	switch (data.type()) {
	case mtpc_webPage:
		return webpage(data.c_webPage());
	case mtpc_webPageEmpty: {
		const auto result = webpage(data.c_webPageEmpty().vid.v);
		if (result->pendingTill > 0) {
			result->pendingTill = -1; // failed
		}
		return result;
	} break;
	case mtpc_webPagePending:
		return webpage(data.c_webPagePending());
	case mtpc_webPageNotModified:
		LOG(("API Error: "
			"webPageNotModified is unexpected in Session::webpage()."));
		return webpage(0);
	}
	Unexpected("Type in Session::webpage().");
}

not_null<WebPageData*> Session::webpage(const MTPDwebPage &data) {
	const auto result = webpage(data.vid.v);
	webpageApplyFields(result, data);
	return result;
}

not_null<WebPageData*> Session::webpage(const MTPDwebPagePending &data) {
	constexpr auto kDefaultPendingTimeout = 60;
	const auto result = webpage(data.vid.v);
	webpageApplyFields(
		result,
		WebPageType::Article,
		QString(),
		QString(),
		QString(),
		QString(),
		TextWithEntities(),
		nullptr,
		nullptr,
		WebPageCollage(),
		0,
		QString(),
		data.vdate.v
			? data.vdate.v
			: (unixtime() + kDefaultPendingTimeout));
	return result;
}

not_null<WebPageData*> Session::webpage(
		WebPageId id,
		const QString &siteName,
		const TextWithEntities &content) {
	return webpage(
		id,
		WebPageType::Article,
		QString(),
		QString(),
		siteName,
		QString(),
		content,
		nullptr,
		nullptr,
		WebPageCollage(),
		0,
		QString(),
		TimeId(0));
}

not_null<WebPageData*> Session::webpage(
		WebPageId id,
		WebPageType type,
		const QString &url,
		const QString &displayUrl,
		const QString &siteName,
		const QString &title,
		const TextWithEntities &description,
		PhotoData *photo,
		DocumentData *document,
		WebPageCollage &&collage,
		int duration,
		const QString &author,
		TimeId pendingTill) {
	const auto result = webpage(id);
	webpageApplyFields(
		result,
		type,
		url,
		displayUrl,
		siteName,
		title,
		description,
		photo,
		document,
		std::move(collage),
		duration,
		author,
		pendingTill);
	return result;
}

void Session::webpageApplyFields(
		not_null<WebPageData*> page,
		const MTPDwebPage &data) {
	auto description = TextWithEntities {
		data.has_description()
			? TextUtilities::Clean(qs(data.vdescription))
			: QString()
	};
	const auto siteName = data.has_site_name()
		? qs(data.vsite_name)
		: QString();
	auto parseFlags = TextParseLinks | TextParseMultiline | TextParseRichText;
	if (siteName == qstr("Twitter") || siteName == qstr("Instagram")) {
		parseFlags |= TextParseHashtags | TextParseMentions;
	}
	TextUtilities::ParseEntities(description, parseFlags);
	const auto pendingTill = TimeId(0);
	webpageApplyFields(
		page,
		ParseWebPageType(data),
		qs(data.vurl),
		qs(data.vdisplay_url),
		siteName,
		data.has_title() ? qs(data.vtitle) : QString(),
		description,
		data.has_photo() ? photo(data.vphoto).get() : nullptr,
		data.has_document() ? document(data.vdocument).get() : nullptr,
		WebPageCollage(data),
		data.has_duration() ? data.vduration.v : 0,
		data.has_author() ? qs(data.vauthor) : QString(),
		pendingTill);
}

void Session::webpageApplyFields(
		not_null<WebPageData*> page,
		WebPageType type,
		const QString &url,
		const QString &displayUrl,
		const QString &siteName,
		const QString &title,
		const TextWithEntities &description,
		PhotoData *photo,
		DocumentData *document,
		WebPageCollage &&collage,
		int duration,
		const QString &author,
		TimeId pendingTill) {
	const auto requestPending = (!page->pendingTill && pendingTill > 0);
	const auto changed = page->applyChanges(
		type,
		url,
		displayUrl,
		siteName,
		title,
		description,
		photo,
		document,
		std::move(collage),
		duration,
		author,
		pendingTill);
	if (requestPending) {
		_session->api().requestWebPageDelayed(page);
	}
	if (changed) {
		notifyWebPageUpdateDelayed(page);
	}
}

not_null<GameData*> Session::game(GameId id) {
	auto i = _games.find(id);
	if (i == _games.cend()) {
		i = _games.emplace(id, std::make_unique<GameData>(id)).first;
	}
	return i->second.get();
}

not_null<GameData*> Session::game(const MTPDgame &data) {
	const auto result = game(data.vid.v);
	gameApplyFields(result, data);
	return result;
}

not_null<GameData*> Session::game(
		GameId id,
		const uint64 &accessHash,
		const QString &shortName,
		const QString &title,
		const QString &description,
		PhotoData *photo,
		DocumentData *document) {
	const auto result = game(id);
	gameApplyFields(
		result,
		accessHash,
		shortName,
		title,
		description,
		photo,
		document);
	return result;
}

void Session::gameConvert(
		not_null<GameData*> original,
		const MTPGame &data) {
	Expects(data.type() == mtpc_game);

	const auto id = data.c_game().vid.v;
	if (original->id != id) {
		auto i = _games.find(id);
		if (i == _games.end()) {
			const auto j = _games.find(original->id);
			Assert(j != _games.end());
			auto owned = std::move(j->second);
			_games.erase(j);
			i = _games.emplace(id, std::move(owned)).first;
		}

		original->id = id;
		original->accessHash = 0;

		if (i->second.get() != original) {
			gameApplyFields(i->second.get(), data.c_game());
		}
	}
	gameApplyFields(original, data.c_game());
}

void Session::gameApplyFields(
		not_null<GameData*> game,
		const MTPDgame &data) {
	gameApplyFields(
		game,
		data.vaccess_hash.v,
		qs(data.vshort_name),
		qs(data.vtitle),
		qs(data.vdescription),
		photo(data.vphoto),
		data.has_document() ? document(data.vdocument).get() : nullptr);
}

void Session::gameApplyFields(
		not_null<GameData*> game,
		const uint64 &accessHash,
		const QString &shortName,
		const QString &title,
		const QString &description,
		PhotoData *photo,
		DocumentData *document) {
	if (game->accessHash) {
		return;
	}
	game->accessHash = accessHash;
	game->shortName = TextUtilities::Clean(shortName);
	game->title = TextUtilities::SingleLine(title);
	game->description = TextUtilities::Clean(description);
	game->photo = photo;
	game->document = document;
	notifyGameUpdateDelayed(game);
}

not_null<PollData*> Session::poll(PollId id) {
	auto i = _polls.find(id);
	if (i == _polls.cend()) {
		i = _polls.emplace(id, std::make_unique<PollData>(id)).first;
	}
	return i->second.get();
}

not_null<PollData*> Session::poll(const MTPPoll &data) {
	return data.match([&](const MTPDpoll &data) {
		const auto id = data.vid.v;
		const auto result = poll(id);
		const auto changed = result->applyChanges(data);
		if (changed) {
			notifyPollUpdateDelayed(result);
		}
		return result;
	});
}

not_null<PollData*> Session::poll(const MTPDmessageMediaPoll &data) {
	const auto result = poll(data.vpoll);
	const auto changed = result->applyResults(data.vresults);
	if (changed) {
		notifyPollUpdateDelayed(result);
	}
	return result;
}

void Session::applyUpdate(const MTPDupdateMessagePoll &update) {
	const auto updated = [&] {
		const auto i = _polls.find(update.vpoll_id.v);
		return (i == end(_polls))
			? nullptr
			: update.has_poll()
			? poll(update.vpoll).get()
			: i->second.get();
	}();
	if (updated && updated->applyResults(update.vresults)) {
		notifyPollUpdateDelayed(updated);
	}
}

void Session::applyUpdate(const MTPDupdateChatParticipants &update) {
	const auto chatId = update.vparticipants.match([](const auto &update) {
		return update.vchat_id.v;
	});
	if (const auto chat = chatLoaded(chatId)) {
		ApplyChatUpdate(chat, update);
		for (const auto user : chat->participants) {
			if (user->botInfo && !user->botInfo->inited) {
				_session->api().requestFullPeer(user);
			}
		}
	}
}

void Session::applyUpdate(const MTPDupdateChatParticipantAdd &update) {
	if (const auto chat = chatLoaded(update.vchat_id.v)) {
		ApplyChatUpdate(chat, update);
	}
}

void Session::applyUpdate(const MTPDupdateChatParticipantDelete &update) {
	if (const auto chat = chatLoaded(update.vchat_id.v)) {
		ApplyChatUpdate(chat, update);
	}
}

void Session::applyUpdate(const MTPDupdateChatParticipantAdmin &update) {
	if (const auto chat = chatLoaded(update.vchat_id.v)) {
		ApplyChatUpdate(chat, update);
	}
}

void Session::applyUpdate(const MTPDupdateChatDefaultBannedRights &update) {
	if (const auto peer = peerLoaded(peerFromMTP(update.vpeer))) {
		if (const auto chat = peer->asChat()) {
			ApplyChatUpdate(chat, update);
		} else if (const auto channel = peer->asChannel()) {
			ApplyChannelUpdate(channel, update);
		} else {
			LOG(("API Error: "
				"User received in updateChatDefaultBannedRights."));
		}
	}
}

not_null<LocationData*> Session::location(const LocationCoords &coords) {
	auto i = _locations.find(coords);
	if (i == _locations.cend()) {
		i = _locations.emplace(
			coords,
			std::make_unique<LocationData>(coords)).first;
	}
	return i->second.get();
}

void Session::registerPhotoItem(
		not_null<const PhotoData*> photo,
		not_null<HistoryItem*> item) {
	_photoItems[photo].insert(item);
}

void Session::unregisterPhotoItem(
		not_null<const PhotoData*> photo,
		not_null<HistoryItem*> item) {
	const auto i = _photoItems.find(photo);
	if (i != _photoItems.end()) {
		auto &items = i->second;
		if (items.remove(item) && items.empty()) {
			_photoItems.erase(i);
		}
	}
}

void Session::registerDocumentItem(
		not_null<const DocumentData*> document,
		not_null<HistoryItem*> item) {
	_documentItems[document].insert(item);
}

void Session::unregisterDocumentItem(
		not_null<const DocumentData*> document,
		not_null<HistoryItem*> item) {
	const auto i = _documentItems.find(document);
	if (i != _documentItems.end()) {
		auto &items = i->second;
		if (items.remove(item) && items.empty()) {
			_documentItems.erase(i);
		}
	}
}

void Session::registerWebPageView(
		not_null<const WebPageData*> page,
		not_null<ViewElement*> view) {
	_webpageViews[page].insert(view);
}

void Session::unregisterWebPageView(
		not_null<const WebPageData*> page,
		not_null<ViewElement*> view) {
	const auto i = _webpageViews.find(page);
	if (i != _webpageViews.end()) {
		auto &items = i->second;
		if (items.remove(view) && items.empty()) {
			_webpageViews.erase(i);
		}
	}
}

void Session::registerWebPageItem(
		not_null<const WebPageData*> page,
		not_null<HistoryItem*> item) {
	_webpageItems[page].insert(item);
}

void Session::unregisterWebPageItem(
		not_null<const WebPageData*> page,
		not_null<HistoryItem*> item) {
	const auto i = _webpageItems.find(page);
	if (i != _webpageItems.end()) {
		auto &items = i->second;
		if (items.remove(item) && items.empty()) {
			_webpageItems.erase(i);
		}
	}
}

void Session::registerGameView(
		not_null<const GameData*> game,
		not_null<ViewElement*> view) {
	_gameViews[game].insert(view);
}

void Session::unregisterGameView(
		not_null<const GameData*> game,
		not_null<ViewElement*> view) {
	const auto i = _gameViews.find(game);
	if (i != _gameViews.end()) {
		auto &items = i->second;
		if (items.remove(view) && items.empty()) {
			_gameViews.erase(i);
		}
	}
}

void Session::registerPollView(
		not_null<const PollData*> poll,
		not_null<ViewElement*> view) {
	_pollViews[poll].insert(view);
}

void Session::unregisterPollView(
		not_null<const PollData*> poll,
		not_null<ViewElement*> view) {
	const auto i = _pollViews.find(poll);
	if (i != _pollViews.end()) {
		auto &items = i->second;
		if (items.remove(view) && items.empty()) {
			_pollViews.erase(i);
		}
	}
}

void Session::registerContactView(
		UserId contactId,
		not_null<ViewElement*> view) {
	if (!contactId) {
		return;
	}
	_contactViews[contactId].insert(view);
}

void Session::unregisterContactView(
		UserId contactId,
		not_null<ViewElement*> view) {
	if (!contactId) {
		return;
	}
	const auto i = _contactViews.find(contactId);
	if (i != _contactViews.end()) {
		auto &items = i->second;
		if (items.remove(view) && items.empty()) {
			_contactViews.erase(i);
		}
	}
}

void Session::registerContactItem(
		UserId contactId,
		not_null<HistoryItem*> item) {
	if (!contactId) {
		return;
	}
	const auto contact = userLoaded(contactId);
	const auto canShare = contact ? contact->canShareThisContact() : false;

	_contactItems[contactId].insert(item);

	if (contact && canShare != contact->canShareThisContact()) {
		Notify::peerUpdatedDelayed(
			contact,
			Notify::PeerUpdate::Flag::UserCanShareContact);
	}

	if (const auto i = _views.find(item); i != _views.end()) {
		for (const auto view : i->second) {
			if (const auto media = view->media()) {
				media->updateSharedContactUserId(contactId);
			}
		}
	}
}

void Session::unregisterContactItem(
		UserId contactId,
		not_null<HistoryItem*> item) {
	if (!contactId) {
		return;
	}
	const auto contact = userLoaded(contactId);
	const auto canShare = contact ? contact->canShareThisContact() : false;

	const auto i = _contactItems.find(contactId);
	if (i != _contactItems.end()) {
		auto &items = i->second;
		if (items.remove(item) && items.empty()) {
			_contactItems.erase(i);
		}
	}

	if (contact && canShare != contact->canShareThisContact()) {
		Notify::peerUpdatedDelayed(
			contact,
			Notify::PeerUpdate::Flag::UserCanShareContact);
	}
}

void Session::registerAutoplayAnimation(
		not_null<::Media::Clip::Reader*> reader,
		not_null<ViewElement*> view) {
	_autoplayAnimations.emplace(reader, view);
}

void Session::unregisterAutoplayAnimation(
		not_null<::Media::Clip::Reader*> reader) {
	_autoplayAnimations.remove(reader);
}

void Session::stopAutoplayAnimations() {
	for (const auto [reader, view] : base::take(_autoplayAnimations)) {
		if (const auto media = view->media()) {
			media->stopAnimation();
		}
	}
}

HistoryItem *Session::findWebPageItem(not_null<WebPageData*> page) const {
	const auto i = _webpageItems.find(page);
	if (i != _webpageItems.end()) {
		for (const auto item : i->second) {
			if (IsServerMsgId(item->id)) {
				return item;
			}
		}
	}
	return nullptr;
}

QString Session::findContactPhone(not_null<UserData*> contact) const {
	const auto result = contact->phone();
	return result.isEmpty()
		? findContactPhone(contact->bareId())
		: App::formatPhone(result);
}

QString Session::findContactPhone(UserId contactId) const {
	const auto i = _contactItems.find(contactId);
	if (i != _contactItems.end()) {
		if (const auto media = (*begin(i->second))->media()) {
			if (const auto contact = media->sharedContact()) {
				return contact->phoneNumber;
			}
		}
	}
	return QString();
}

bool Session::hasPendingWebPageGamePollNotification() const {
	return !_webpagesUpdated.empty()
		|| !_gamesUpdated.empty()
		|| !_pollsUpdated.empty();
}

void Session::notifyWebPageUpdateDelayed(not_null<WebPageData*> page) {
	const auto invoke = !hasPendingWebPageGamePollNotification();
	_webpagesUpdated.insert(page);
	if (invoke) {
		crl::on_main(_session, [=] { sendWebPageGamePollNotifications(); });
	}
}

void Session::notifyGameUpdateDelayed(not_null<GameData*> game) {
	const auto invoke = !hasPendingWebPageGamePollNotification();
	_gamesUpdated.insert(game);
	if (invoke) {
		crl::on_main(_session, [=] { sendWebPageGamePollNotifications(); });
	}
}

void Session::notifyPollUpdateDelayed(not_null<PollData*> poll) {
	const auto invoke = !hasPendingWebPageGamePollNotification();
	_pollsUpdated.insert(poll);
	if (invoke) {
		crl::on_main(_session, [=] { sendWebPageGamePollNotifications(); });
	}
}

void Session::sendWebPageGamePollNotifications() {
	for (const auto page : base::take(_webpagesUpdated)) {
		const auto i = _webpageViews.find(page);
		if (i != _webpageViews.end()) {
			for (const auto view : i->second) {
				requestViewResize(view);
			}
		}
	}
	for (const auto game : base::take(_gamesUpdated)) {
		if (const auto i = _gameViews.find(game); i != _gameViews.end()) {
			for (const auto view : i->second) {
				requestViewResize(view);
			}
		}
	}
	for (const auto poll : base::take(_pollsUpdated)) {
		if (const auto i = _pollViews.find(poll); i != _pollViews.end()) {
			for (const auto view : i->second) {
				requestViewResize(view);
			}
		}
	}
}

void Session::registerItemView(not_null<ViewElement*> view) {
	_views[view->data()].push_back(view);
}

void Session::unregisterItemView(not_null<ViewElement*> view) {
	const auto i = _views.find(view->data());
	if (i != end(_views)) {
		auto &list = i->second;
		list.erase(ranges::remove(list, view), end(list));
		if (list.empty()) {
			_views.erase(i);
		}
	}
	if (App::hoveredItem() == view) {
		App::hoveredItem(nullptr);
	}
	if (App::pressedItem() == view) {
		App::pressedItem(nullptr);
	}
	if (App::hoveredLinkItem() == view) {
		App::hoveredLinkItem(nullptr);
	}
	if (App::pressedLinkItem() == view) {
		App::pressedLinkItem(nullptr);
	}
	if (App::mousedItem() == view) {
		App::mousedItem(nullptr);
	}
}

not_null<Feed*> Session::feed(FeedId id) {
	if (const auto result = feedLoaded(id)) {
		return result;
	}
	const auto [it, ok] = _feeds.emplace(
		id,
		std::make_unique<Feed>(this, id));
	return it->second.get();
}

Feed *Session::feedLoaded(FeedId id) {
	const auto it = _feeds.find(id);
	return (it == end(_feeds)) ? nullptr : it->second.get();
}

void Session::setDefaultFeedId(FeedId id) {
	_defaultFeedId = id;
}

FeedId Session::defaultFeedId() const {
	return _defaultFeedId.current();
}

rpl::producer<FeedId> Session::defaultFeedIdValue() const {
	return _defaultFeedId.value();
}

void Session::requestNotifySettings(not_null<PeerData*> peer) {
	if (peer->notifySettingsUnknown()) {
		_session->api().requestNotifySettings(
			MTP_inputNotifyPeer(peer->input));
	}
	if (defaultNotifySettings(peer).settingsUnknown()) {
		_session->api().requestNotifySettings(peer->isUser()
			? MTP_inputNotifyUsers()
			: (peer->isChat() || peer->isMegagroup())
			? MTP_inputNotifyChats()
			: MTP_inputNotifyBroadcasts());
	}
}

void Session::applyNotifySetting(
		const MTPNotifyPeer &notifyPeer,
		const MTPPeerNotifySettings &settings) {
	switch (notifyPeer.type()) {
	case mtpc_notifyUsers: {
		if (_defaultUserNotifySettings.change(settings)) {
			_defaultUserNotifyUpdates.fire({});

			enumerateUsers([&](not_null<UserData*> user) {
				if (!user->notifySettingsUnknown()
					&& ((!user->notifyMuteUntil()
						&& _defaultUserNotifySettings.muteUntil())
						|| (!user->notifySilentPosts()
							&& _defaultUserNotifySettings.silentPosts()))) {
					updateNotifySettingsLocal(user);
				}
			});
		}
	} break;
	case mtpc_notifyChats: {
		if (_defaultChatNotifySettings.change(settings)) {
			_defaultChatNotifyUpdates.fire({});

			enumerateGroups([&](not_null<PeerData*> peer) {
				if (!peer->notifySettingsUnknown()
					&& ((!peer->notifyMuteUntil()
						&& _defaultChatNotifySettings.muteUntil())
						|| (!peer->notifySilentPosts()
							&& _defaultChatNotifySettings.silentPosts()))) {
					updateNotifySettingsLocal(peer);
				}
			});
		}
	} break;
	case mtpc_notifyBroadcasts: {
		if (_defaultBroadcastNotifySettings.change(settings)) {
			_defaultBroadcastNotifyUpdates.fire({});

			enumerateChannels([&](not_null<ChannelData*> channel) {
				if (!channel->notifySettingsUnknown()
					&& ((!channel->notifyMuteUntil()
						&& _defaultBroadcastNotifySettings.muteUntil())
						|| (!channel->notifySilentPosts()
							&& _defaultBroadcastNotifySettings.silentPosts()))) {
					updateNotifySettingsLocal(channel);
				}
			});
		}
	} break;
	case mtpc_notifyPeer: {
		const auto &data = notifyPeer.c_notifyPeer();
		if (const auto peer = peerLoaded(peerFromMTP(data.vpeer))) {
			if (peer->notifyChange(settings)) {
				updateNotifySettingsLocal(peer);
			}
		}
	} break;
	}
}

void Session::updateNotifySettings(
		not_null<PeerData*> peer,
		std::optional<int> muteForSeconds,
		std::optional<bool> silentPosts) {
	if (peer->notifyChange(muteForSeconds, silentPosts)) {
		updateNotifySettingsLocal(peer);
		_session->api().updateNotifySettingsDelayed(peer);
	}
}

bool Session::notifyIsMuted(
		not_null<const PeerData*> peer,
		TimeMs *changesIn) const {
	const auto resultFromUntil = [&](TimeId until) {
		const auto now = unixtime();
		const auto result = (until > now) ? (until - now) : 0;
		if (changesIn) {
			*changesIn = (result > 0)
				? std::min(result * TimeMs(1000), kMaxNotifyCheckDelay)
				: kMaxNotifyCheckDelay;
		}
		return (result > 0);
	};
	if (const auto until = peer->notifyMuteUntil()) {
		return resultFromUntil(*until);
	}
	const auto &settings = defaultNotifySettings(peer);
	if (const auto until = settings.muteUntil()) {
		return resultFromUntil(*until);
	}
	return true;
}

bool Session::notifySilentPosts(not_null<const PeerData*> peer) const {
	if (const auto silent = peer->notifySilentPosts()) {
		return *silent;
	}
	const auto &settings = defaultNotifySettings(peer);
	if (const auto silent = settings.silentPosts()) {
		return *silent;
	}
	return false;
}

bool Session::notifyMuteUnknown(not_null<const PeerData*> peer) const {
	if (peer->notifySettingsUnknown()) {
		return true;
	} else if (const auto nonDefault = peer->notifyMuteUntil()) {
		return false;
	}
	return defaultNotifySettings(peer).settingsUnknown();
}

bool Session::notifySilentPostsUnknown(
		not_null<const PeerData*> peer) const {
	if (peer->notifySettingsUnknown()) {
		return true;
	} else if (const auto nonDefault = peer->notifySilentPosts()) {
		return false;
	}
	return defaultNotifySettings(peer).settingsUnknown();
}

bool Session::notifySettingsUnknown(not_null<const PeerData*> peer) const {
	return notifyMuteUnknown(peer) || notifySilentPostsUnknown(peer);
}

rpl::producer<> Session::defaultUserNotifyUpdates() const {
	return _defaultUserNotifyUpdates.events();
}

rpl::producer<> Session::defaultChatNotifyUpdates() const {
	return _defaultChatNotifyUpdates.events();
}

rpl::producer<> Session::defaultBroadcastNotifyUpdates() const {
	return _defaultBroadcastNotifyUpdates.events();
}

rpl::producer<> Session::defaultNotifyUpdates(
		not_null<const PeerData*> peer) const {
	return peer->isUser()
		? defaultUserNotifyUpdates()
		: (peer->isChat() || peer->isMegagroup())
		? defaultChatNotifyUpdates()
		: defaultBroadcastNotifyUpdates();
}

void Session::serviceNotification(
		const TextWithEntities &message,
		const MTPMessageMedia &media) {
	const auto date = unixtime();
	if (!userLoaded(ServiceUserId)) {
		user(MTP_user(
			MTP_flags(
				MTPDuser::Flag::f_first_name
				| MTPDuser::Flag::f_phone
				| MTPDuser::Flag::f_status
				| MTPDuser::Flag::f_verified),
			MTP_int(ServiceUserId),
			MTPlong(),
			MTP_string("Telegram"),
			MTPstring(),
			MTPstring(),
			MTP_string("42777"),
			MTP_userProfilePhotoEmpty(),
			MTP_userStatusRecently(),
			MTPint(),
			MTPstring(),
			MTPstring(),
			MTPstring()));
	}
	const auto history = this->history(peerFromUser(ServiceUserId));
	if (!history->lastMessageKnown()) {
		_session->api().requestDialogEntry(history, [=] {
			insertCheckedServiceNotification(message, media, date);
		});
	} else {
		insertCheckedServiceNotification(message, media, date);
	}
}

void Session::checkNewAuthorization() {
	_newAuthorizationChecks.fire({});
}

rpl::producer<> Session::newAuthorizationChecks() const {
	return _newAuthorizationChecks.events();
}

void Session::insertCheckedServiceNotification(
		const TextWithEntities &message,
		const MTPMessageMedia &media,
		TimeId date) {
	const auto history = this->history(peerFromUser(ServiceUserId));
	if (!history->isReadyFor(ShowAtUnreadMsgId)) {
		history->setUnreadCount(0);
		history->getReadyFor(ShowAtTheEndMsgId);
	}
	const auto flags = MTPDmessage::Flag::f_entities
		| MTPDmessage::Flag::f_from_id
		| MTPDmessage_ClientFlag::f_clientside_unread;
	auto sending = TextWithEntities(), left = message;
	while (TextUtilities::CutPart(sending, left, MaxMessageSize)) {
		addNewMessage(
			MTP_message(
				MTP_flags(flags),
				MTP_int(clientMsgId()),
				MTP_int(ServiceUserId),
				MTP_peerUser(MTP_int(_session->userId())),
				MTPnullFwdHeader,
				MTPint(),
				MTPint(),
				MTP_int(date),
				MTP_string(sending.text),
				media,
				MTPnullMarkup,
				TextUtilities::EntitiesToMTP(sending.entities),
				MTPint(),
				MTPint(),
				MTPstring(),
				MTPlong()),
			NewMessageUnread);
	}
	sendHistoryChangeNotifications();
}

void Session::setMimeForwardIds(MessageIdsList &&list) {
	_mimeForwardIds = std::move(list);
}

MessageIdsList Session::takeMimeForwardIds() {
	return std::move(_mimeForwardIds);
}

void Session::setProxyPromoted(PeerData *promoted) {
	if (_proxyPromoted != promoted) {
		if (const auto history = historyLoaded(_proxyPromoted)) {
			history->cacheProxyPromoted(false);
		}
		const auto old = std::exchange(_proxyPromoted, promoted);
		if (_proxyPromoted) {
			const auto history = this->history(_proxyPromoted);
			history->cacheProxyPromoted(true);
			history->requestChatListMessage();
			Notify::peerUpdatedDelayed(
				_proxyPromoted,
				Notify::PeerUpdate::Flag::ChannelPromotedChanged);
		}
		if (old) {
			Notify::peerUpdatedDelayed(
				old,
				Notify::PeerUpdate::Flag::ChannelPromotedChanged);
		}
	}
}

PeerData *Session::proxyPromoted() const {
	return _proxyPromoted;
}

bool Session::updateWallpapers(const MTPaccount_WallPapers &data) {
	return data.match([&](const MTPDaccount_wallPapers &data) {
		setWallpapers(data.vwallpapers.v, data.vhash.v);
		return true;
	}, [&](const MTPDaccount_wallPapersNotModified &) {
		return false;
	});
}

void Session::setWallpapers(const QVector<MTPWallPaper> &data, int32 hash) {
	_wallpapersHash = hash;

	_wallpapers.clear();
	_wallpapers.reserve(data.size() + 1);

	const auto defaultBackground = Images::Create(
		qsl(":/gui/art/bg.jpg"),
		"JPG");
	if (defaultBackground) {
		_wallpapers.push_back({
			Window::Theme::kDefaultBackground,
			0ULL, // access_hash
			MTPDwallPaper::Flags(0),
			QString(), // slug
			defaultBackground
		});
	}
	const auto oldBackground = Images::Create(
		qsl(":/gui/art/bg_initial.jpg"),
		"JPG");
	if (oldBackground) {
		_wallpapers.push_back({
			Window::Theme::kInitialBackground,
			0ULL, // access_hash
			MTPDwallPaper::Flags(0),
			QString(), // slug
			oldBackground
		});
	}
	for (const auto &paper : data) {
		paper.match([&](const MTPDwallPaper &paper) {
			const auto document = this->document(paper.vdocument);
			if (document->checkWallPaperProperties()) {
				_wallpapers.push_back({
					paper.vid.v,
					paper.vaccess_hash.v,
					paper.vflags.v,
					qs(paper.vslug),
					document->thumb,
					document,
				});
			}
		});
	}
}

const std::vector<WallPaper> &Session::wallpapers() const {
	return _wallpapers;
}

int32 Session::wallpapersHash() const {
	return _wallpapersHash;
}

void Session::clearLocalStorage() {
	clear();

	_cache->close();
	_cache->clear();
}

} // namespace Data
