/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#define _ALLOW_COROUTINE_ABI_MISMATCH
#include "platform/win/notifications_manager_win.h"

#include "window/notifications_utilities.h"
#include "window/window_session_controller.h"
#include "base/platform/win/base_windows_co_task_mem.h"
#include "base/platform/win/base_windows_winrt.h"
#include "base/platform/base_platform_info.h"
#include "base/platform/win/wrl/wrl_module_h.h"
#include "base/qthelp_url.h"
#include "platform/win/windows_app_user_model_id.h"
#include "platform/win/windows_toast_activator.h"
#include "platform/win/windows_dlls.h"
#include "platform/win/specific_win.h"
#include "data/data_forum_topic.h"
#include "history/history.h"
#include "history/history_item.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "windows_quiethours_h.h"
#include "styles/style_chat.h"

#include <QtCore/QOperatingSystemVersion>

#include <Shobjidl.h>
#include <shellapi.h>
#include <strsafe.h>

#ifndef __MINGW32__
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

HICON qt_pixmapToWinHICON(const QPixmap &);

using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::Data::Xml::Dom;
using namespace winrt::Windows::Foundation;
using winrt::com_ptr;
#endif // !__MINGW32__

namespace Platform {
namespace Notifications {

#ifndef __MINGW32__
namespace {

constexpr auto kQuerySettingsEachMs = 1000;

crl::time LastSettingsQueryMs/* = 0*/;

[[nodiscard]] bool ShouldQuerySettings() {
	const auto now = crl::now();
	if (LastSettingsQueryMs > 0 && now <= LastSettingsQueryMs + kQuerySettingsEachMs) {
		return false;
	}
	LastSettingsQueryMs = now;
	return true;
}

[[nodiscard]] std::wstring NotificationTemplate(
		QString id,
		Window::Notifications::Manager::DisplayOptions options) {
	const auto wid = id.replace('&', "&amp;").toStdWString();
	const auto fastReply = LR"(
		<input id="fastReply" type="text" placeHolderContent=""/>
		<action
			content="Send"
			arguments="action=reply&amp;)" + wid + LR"("
			activationType="background"
			imageUri=""
			hint-inputId="fastReply"/>
)";
	const auto markAsRead = LR"(
        <action
            content=""
            arguments="action=mark&amp;)" + wid + LR"("
            activationType="background"/>
)";
	const auto actions = (options.hideReplyButton ? L"" : fastReply)
		+ (options.hideMarkAsRead ? L"" : markAsRead);
	return LR"(
<toast launch="action=open&amp;)" + wid + LR"(">
	<visual>
		<binding template="ToastGeneric">
			<image placement="appLogoOverride" hint-crop="circle" src=""/>
			<text hint-maxLines="1"></text>
			<text></text>
			<text></text>
		</binding>
	</visual>
)" + (actions.empty()
	? L""
	: (L"<actions>" + actions + L"</actions>")) + LR"(
	<audio silent="true"/>
</toast>
)";
}

bool init() {
	if (!IsWindows8OrGreater()) {
		return false;
	}
	if ((Dlls::SetCurrentProcessExplicitAppUserModelID == nullptr)
		|| !base::WinRT::Supported()) {
		return false;
	}

	{
		using namespace Microsoft::WRL;
		const auto hr = Module<OutOfProc>::GetModule().RegisterObjects();
		if (!SUCCEEDED(hr)) {
			LOG(("App Error: Object registration failed."));
		}
	}
	if (!AppUserModelId::validateShortcut()) {
		LOG(("App Error: Shortcut validation failed."));
		return false;
	}

	auto appUserModelId = AppUserModelId::getId();
	if (!SUCCEEDED(Dlls::SetCurrentProcessExplicitAppUserModelID(appUserModelId))) {
		return false;
	}
	return true;
}

// Throws.
void SetNodeValueString(
		const XmlDocument &xml,
		const IXmlNode &node,
		const std::wstring &text) {
	node.AppendChild(xml.CreateTextNode(text).as<IXmlNode>());
}

// Throws.
void SetAudioSilent(const XmlDocument &toastXml) {
	const auto nodeList = toastXml.GetElementsByTagName(L"audio");
	if (const auto audioNode = nodeList.Item(0)) {
		audioNode.as<IXmlElement>().SetAttribute(L"silent", L"true");
	} else {
		auto audioElement = toastXml.CreateElement(L"audio");
		audioElement.SetAttribute(L"silent", L"true");
		auto nodeList = toastXml.GetElementsByTagName(L"toast");
		nodeList.Item(0).AppendChild(audioElement.as<IXmlNode>());
	}
}

// Throws.
void SetImageSrc(const XmlDocument &toastXml, const std::wstring &path) {
	const auto nodeList = toastXml.GetElementsByTagName(L"image");
	const auto attributes = nodeList.Item(0).Attributes();
	return SetNodeValueString(
		toastXml,
		attributes.GetNamedItem(L"src"),
		L"file:///" + path);
}

// Throws.
void SetReplyIconSrc(const XmlDocument &toastXml, const std::wstring &path) {
	const auto nodeList = toastXml.GetElementsByTagName(L"action");
	const auto length = int(nodeList.Length());
	for (auto i = 0; i != length; ++i) {
		const auto attributes = nodeList.Item(i).Attributes();
		if (const auto uri = attributes.GetNamedItem(L"imageUri")) {
			return SetNodeValueString(toastXml, uri, L"file:///" + path);
		}
	}
}

// Throws.
void SetReplyPlaceholder(
		const XmlDocument &toastXml,
		const std::wstring &placeholder) {
	const auto nodeList = toastXml.GetElementsByTagName(L"input");
	const auto attributes = nodeList.Item(0).Attributes();
	return SetNodeValueString(
		toastXml,
		attributes.GetNamedItem(L"placeHolderContent"),
		placeholder);
}

// Throws.
void SetAction(const XmlDocument &toastXml, const QString &id) {
	auto nodeList = toastXml.GetElementsByTagName(L"toast");
	if (const auto toast = nodeList.Item(0).try_as<XmlElement>()) {
		toast.SetAttribute(L"launch", L"action=open&" + id.toStdWString());
	}
}

// Throws.
void SetMarkAsReadText(
		const XmlDocument &toastXml,
		const std::wstring &text) {
	const auto nodeList = toastXml.GetElementsByTagName(L"action");
	const auto length = int(nodeList.Length());
	for (auto i = 0; i != length; ++i) {
		const auto attributes = nodeList.Item(i).Attributes();
		if (!attributes.GetNamedItem(L"imageUri")) {
			return SetNodeValueString(
				toastXml,
				attributes.GetNamedItem(L"content"),
				text);
		}
	}
}

auto Checked = false;
auto InitSucceeded = false;

void Check() {
	InitSucceeded = init();
}

bool QuietHoursEnabled = false;
DWORD QuietHoursValue = 0;

[[nodiscard]] bool UseQuietHoursRegistryEntry() {
	static const bool result = [] {
		const auto version = QOperatingSystemVersion::current();

		// At build 17134 (Redstone 4) the "Quiet hours" was replaced
		// by "Focus assist" and it looks like it doesn't use registry.
		return (version.majorVersion() == 10)
			&& (version.minorVersion() == 0)
			&& (version.microVersion() < 17134);
	}();
	return result;
}

// Thanks https://stackoverflow.com/questions/35600128/get-windows-quiet-hours-from-win32-or-c-sharp-api
void QueryQuietHours() {
	if (!UseQuietHoursRegistryEntry()) {
		// There are quiet hours in Windows starting from Windows 8.1
		// But there were several reports about the notifications being shut
		// down according to the registry while no quiet hours were enabled.
		// So we try this method only starting with Windows 10.
		return;
	}

	LPCWSTR lpKeyName = L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings";
	LPCWSTR lpValueName = L"NOC_GLOBAL_SETTING_TOASTS_ENABLED";
	HKEY key;
	auto result = RegOpenKeyEx(HKEY_CURRENT_USER, lpKeyName, 0, KEY_READ, &key);
	if (result != ERROR_SUCCESS) {
		return;
	}

	DWORD value = 0, type = 0, size = sizeof(value);
	result = RegQueryValueEx(key, lpValueName, 0, &type, (LPBYTE)&value, &size);
	RegCloseKey(key);

	auto quietHoursEnabled = (result == ERROR_SUCCESS) && (value == 0);
	if (QuietHoursEnabled != quietHoursEnabled) {
		QuietHoursEnabled = quietHoursEnabled;
		QuietHoursValue = value;
		LOG(("Quiet hours changed, entry value: %1").arg(value));
	} else if (QuietHoursValue != value) {
		QuietHoursValue = value;
		LOG(("Quiet hours value changed, was value: %1, entry value: %2").arg(QuietHoursValue).arg(value));
	}
}

bool FocusAssistBlocks = false;

// Thanks https://www.withinrafael.com/2019/09/19/determine-if-your-app-is-in-a-focus-assist-profiles-priority-list/
void QueryFocusAssist() {
	const auto quietHoursSettings = base::WinRT::TryCreateInstance<
		IQuietHoursSettings
	>(CLSID_QuietHoursSettings, CLSCTX_LOCAL_SERVER);
	if (!quietHoursSettings) {
		return;
	}

	auto profileId = base::CoTaskMemString();
	auto hr = quietHoursSettings->get_UserSelectedProfile(profileId.put());
	if (FAILED(hr) || !profileId) {
		return;
	}
	const auto profileName = QString::fromWCharArray(profileId.data());
	if (profileName.endsWith(".alarmsonly", Qt::CaseInsensitive)) {
		if (!FocusAssistBlocks) {
			LOG(("Focus Assist: Alarms Only."));
			FocusAssistBlocks = true;
		}
		return;
	} else if (!profileName.endsWith(".priorityonly", Qt::CaseInsensitive)) {
		if (!profileName.endsWith(".unrestricted", Qt::CaseInsensitive)) {
			LOG(("Focus Assist Warning: Unknown profile '%1'"
				).arg(profileName));
		}
		if (FocusAssistBlocks) {
			LOG(("Focus Assist: Unrestricted."));
			FocusAssistBlocks = false;
		}
		return;
	}
	const auto appUserModelId = std::wstring(AppUserModelId::getId());
	auto blocked = true;
	const auto guard = gsl::finally([&] {
		if (FocusAssistBlocks != blocked) {
			LOG(("Focus Assist: %1, AppUserModelId: %2, Blocks: %3"
				).arg(profileName
				).arg(QString::fromStdWString(appUserModelId)
				).arg(Logs::b(blocked)));
			FocusAssistBlocks = blocked;
		}
	});

	com_ptr<IQuietHoursProfile> profile;
	hr = quietHoursSettings->GetProfile(profileId.data(), profile.put());
	if (FAILED(hr) || !profile) {
		return;
	}

	auto apps = base::CoTaskMemStringArray();
	hr = profile->GetAllowedApps(apps.put_size(), apps.put());
	if (FAILED(hr) || !apps) {
		return;
	}
	for (const auto &app : apps) {
		if (app && app.data() == appUserModelId) {
			blocked = false;
			break;
		}
	}
}

QUERY_USER_NOTIFICATION_STATE UserNotificationState
	= QUNS_ACCEPTS_NOTIFICATIONS;

void QueryUserNotificationState() {
	if (Dlls::SHQueryUserNotificationState != nullptr) {
		QUERY_USER_NOTIFICATION_STATE state;
		if (SUCCEEDED(Dlls::SHQueryUserNotificationState(&state))) {
			UserNotificationState = state;
		}
	}
}

void QuerySystemNotificationSettings() {
	if (!ShouldQuerySettings()) {
		return;
	}
	QueryQuietHours();
	QueryFocusAssist();
	QueryUserNotificationState();
}

bool SkipSoundForCustom() {
	QuerySystemNotificationSettings();

	return (UserNotificationState == QUNS_NOT_PRESENT)
		|| (UserNotificationState == QUNS_PRESENTATION_MODE)
		|| Core::App().screenIsLocked();
}

bool SkipFlashBounceForCustom() {
	return SkipToastForCustom();
}

} // namespace
#endif // !__MINGW32__

void MaybePlaySoundForCustom(Fn<void()> playSound) {
	if (!SkipSoundForCustom()) {
		playSound();
	}
}

bool SkipToastForCustom() {
	QuerySystemNotificationSettings();

	return (UserNotificationState == QUNS_PRESENTATION_MODE)
		|| (UserNotificationState == QUNS_RUNNING_D3D_FULL_SCREEN);
}

void MaybeFlashBounceForCustom(Fn<void()> flashBounce) {
	if (!SkipFlashBounceForCustom()) {
		flashBounce();
	}
}

bool WaitForInputForCustom() {
	QuerySystemNotificationSettings();

	return UserNotificationState != QUNS_BUSY;
}

bool Supported() {
#ifndef __MINGW32__
	if (!Checked) {
		Checked = true;
		Check();
	}
	return InitSucceeded;
#endif // !__MINGW32__

	return false;
}

bool Enforced() {
	return false;
}

bool ByDefault() {
	return false;
}

void Create(Window::Notifications::System *system) {
#ifndef __MINGW32__
	if (Core::App().settings().nativeNotifications() && Supported()) {
		auto result = std::make_unique<Manager>(system);
		if (result->init()) {
			system->setManager(std::move(result));
			return;
		}
	}
#endif // !__MINGW32__
	system->setManager(nullptr);
}

#ifndef __MINGW32__
class Manager::Private {
public:
	explicit Private(Manager *instance);
	bool init();

	bool showNotification(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		Ui::PeerUserpicView &userpicView,
		MsgId msgId,
		const QString &title,
		const QString &subtitle,
		const QString &msg,
		DisplayOptions options);
	void clearAll();
	void clearFromItem(not_null<HistoryItem*> item);
	void clearFromTopic(not_null<Data::ForumTopic*> topic);
	void clearFromHistory(not_null<History*> history);
	void clearFromSession(not_null<Main::Session*> session);
	void beforeNotificationActivated(NotificationId id);
	void afterNotificationActivated(
		NotificationId id,
		not_null<Window::SessionController*> window);
	void clearNotification(NotificationId id);

	void handleActivation(const ToastActivation &activation);

	~Private();

private:
	bool showNotificationInTryCatch(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		Ui::PeerUserpicView &userpicView,
		MsgId msgId,
		const QString &title,
		const QString &subtitle,
		const QString &msg,
		DisplayOptions options);
	void tryHide(const ToastNotification &notification);
	[[nodiscard]] std::wstring ensureSendButtonIcon();

	Window::Notifications::CachedUserpics _cachedUserpics;
	std::wstring _sendButtonIconPath;

	std::shared_ptr<Manager*> _guarded;
	ToastNotifier _notifier = nullptr;

	base::flat_map<
		ContextId,
		base::flat_map<MsgId, ToastNotification>> _notifications;
	rpl::lifetime _lifetime;

};

Manager::Private::Private(Manager *instance)
: _guarded(std::make_shared<Manager*>(instance)) {
	ToastActivations(
	) | rpl::start_with_next([=](const ToastActivation &activation) {
		handleActivation(activation);
	}, _lifetime);
}

bool Manager::Private::init() {
	return base::WinRT::Try([&] {
		_notifier = ToastNotificationManager::CreateToastNotifier(
			AppUserModelId::getId());
	});
}

Manager::Private::~Private() {
	clearAll();

	_notifications.clear();
	_notifier = nullptr;
}

void Manager::Private::clearAll() {
	if (!_notifier) {
		return;
	}

	for (const auto &[key, notifications] : base::take(_notifications)) {
		for (const auto &[msgId, notification] : notifications) {
			tryHide(notification);
		}
	}
}

void Manager::Private::clearFromItem(not_null<HistoryItem*> item) {
	if (!_notifier) {
		return;
	}

	auto i = _notifications.find(ContextId{
		.sessionId = item->history()->session().uniqueId(),
		.peerId = item->history()->peer->id,
		.topicRootId = item->topicRootId(),
	});
	if (i == _notifications.cend()) {
		return;
	}
	const auto j = i->second.find(item->id);
	if (j == end(i->second)) {
		return;
	}
	const auto taken = std::exchange(j->second, nullptr);
	i->second.erase(j);
	if (i->second.empty()) {
		_notifications.erase(i);
	}
	tryHide(taken);
}

void Manager::Private::clearFromTopic(not_null<Data::ForumTopic*> topic) {
	if (!_notifier) {
		return;
	}

	const auto i = _notifications.find(ContextId{
		.sessionId = topic->session().uniqueId(),
		.peerId = topic->history()->peer->id,
		.topicRootId = topic->rootId(),
	});
	if (i != _notifications.cend()) {
		const auto temp = base::take(i->second);
		_notifications.erase(i);

		for (const auto &[msgId, notification] : temp) {
			tryHide(notification);
		}
	}
}

void Manager::Private::clearFromHistory(not_null<History*> history) {
	if (!_notifier) {
		return;
	}

	const auto sessionId = history->session().uniqueId();
	const auto peerId = history->peer->id;
	auto i = _notifications.lower_bound(ContextId{
		.sessionId = sessionId,
		.peerId = peerId,
	});
	while (i != _notifications.cend()
		&& i->first.sessionId == sessionId
		&& i->first.peerId == peerId) {
		const auto temp = base::take(i->second);
		i = _notifications.erase(i);

		for (const auto &[msgId, notification] : temp) {
			tryHide(notification);
		}
	}
}

void Manager::Private::clearFromSession(not_null<Main::Session*> session) {
	if (!_notifier) {
		return;
	}

	const auto sessionId = session->uniqueId();
	auto i = _notifications.lower_bound(ContextId{
		.sessionId = sessionId,
	});
	while (i != _notifications.cend() && i->first.sessionId == sessionId) {
		const auto temp = base::take(i->second);
		i = _notifications.erase(i);

		for (const auto &[msgId, notification] : temp) {
			tryHide(notification);
		}
	}
}

void Manager::Private::beforeNotificationActivated(NotificationId id) {
	clearNotification(id);
}

void Manager::Private::afterNotificationActivated(
		NotificationId id,
		not_null<Window::SessionController*> window) {
	SetForegroundWindow(window->widget()->psHwnd());
}

void Manager::Private::clearNotification(NotificationId id) {
	auto i = _notifications.find(id.contextId);
	if (i != _notifications.cend()) {
		i->second.remove(id.msgId);
		if (i->second.empty()) {
			_notifications.erase(i);
		}
	}
}

void Manager::Private::handleActivation(const ToastActivation &activation) {
	const auto parsed = qthelp::url_parse_params(activation.args);
	const auto pid = parsed.value("pid").toULong();
	const auto my = GetCurrentProcessId();
	if (pid != my) {
		DEBUG_LOG(("Toast Info: "
			"Got activation \"%1\", my %2, activating %3."
			).arg(activation.args
			).arg(my
			).arg(pid));
		const auto processId = pid;
		const auto windowId = 0; // Activate some window.
		Platform::ActivateOtherProcess(processId, windowId);
		return;
	}
	const auto action = parsed.value("action");
	const auto id = NotificationId{
		.contextId = ContextId{
			.sessionId = parsed.value("session").toULongLong(),
			.peerId = PeerId(parsed.value("peer").toULongLong()),
			.topicRootId = MsgId(parsed.value("topic").toLongLong())
		},
		.msgId = MsgId(parsed.value("msg").toLongLong()),
	};
	if (!id.contextId.sessionId || !id.contextId.peerId || !id.msgId) {
		DEBUG_LOG(("Toast Info: Got activation \"%1\", my %1, skipping."
			).arg(activation.args
			).arg(pid));
		return;
	}
	DEBUG_LOG(("Toast Info: Got activation \"%1\", my %1, handling."
		).arg(activation.args
		).arg(pid));
	auto text = TextWithTags();
	for (const auto &entry : activation.input) {
		if (entry.key == "fastReply") {
			text.text = entry.value;
		}
	}
	const auto i = _notifications.find(id.contextId);
	if (i == _notifications.cend() || !i->second.contains(id.msgId)) {
		return;
	}

	const auto manager = *_guarded;
	if (action == "reply") {
		manager->notificationReplied(id, text);
	} else if (action == "mark") {
		manager->notificationReplied(id, TextWithTags());
	} else {
		manager->notificationActivated(id, text);
	}
}

bool Manager::Private::showNotification(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		Ui::PeerUserpicView &userpicView,
		MsgId msgId,
		const QString &title,
		const QString &subtitle,
		const QString &msg,
		DisplayOptions options) {
	if (!_notifier) {
		return false;
	}

	return base::WinRT::Try([&] {
		return showNotificationInTryCatch(
			peer,
			topicRootId,
			userpicView,
			msgId,
			title,
			subtitle,
			msg,
			options);
	}).value_or(false);
}

std::wstring Manager::Private::ensureSendButtonIcon() {
	if (_sendButtonIconPath.empty()) {
		const auto path = cWorkingDir() + u"tdata/temp/fast_reply.png"_q;
		st::historySendIcon.instance(Qt::white, 300).save(path, "PNG");
		_sendButtonIconPath = path.toStdWString();
	}
	return _sendButtonIconPath;
}

bool Manager::Private::showNotificationInTryCatch(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		Ui::PeerUserpicView &userpicView,
		MsgId msgId,
		const QString &title,
		const QString &subtitle,
		const QString &msg,
		DisplayOptions options) {
	const auto withSubtitle = !subtitle.isEmpty();
	auto toastXml = XmlDocument();

	const auto key = ContextId{
		.sessionId = peer->session().uniqueId(),
		.peerId = peer->id,
		.topicRootId = topicRootId,
	};
	const auto notificationId = NotificationId{
		.contextId = key,
		.msgId = msgId
	};
	const auto idString = u"pid=%1&session=%2&peer=%3&topic=%4&msg=%5"_q
		.arg(GetCurrentProcessId())
		.arg(key.sessionId)
		.arg(key.peerId.value)
		.arg(topicRootId.bare)
		.arg(msgId.bare);

	const auto modern = Platform::IsWindows10OrGreater();
	if (modern) {
		toastXml.LoadXml(NotificationTemplate(idString, options));
	} else {
		toastXml = ToastNotificationManager::GetTemplateContent(
			(withSubtitle
				? ToastTemplateType::ToastImageAndText04
				: ToastTemplateType::ToastImageAndText02));
		SetAudioSilent(toastXml);
		SetAction(toastXml, idString);
	}

	const auto userpicKey = options.hideNameAndPhoto
		? InMemoryKey()
		: peer->userpicUniqueKey(userpicView);
	const auto userpicPath = _cachedUserpics.get(
		userpicKey,
		peer,
		userpicView);
	const auto userpicPathWide = QDir::toNativeSeparators(
		userpicPath).toStdWString();
	if (modern && !options.hideReplyButton) {
		SetReplyIconSrc(toastXml, ensureSendButtonIcon());
		SetReplyPlaceholder(
			toastXml,
			tr::lng_message_ph(tr::now).toStdWString());
	}
	if (modern && !options.hideMarkAsRead) {
		SetMarkAsReadText(
			toastXml,
			tr::lng_context_mark_read(tr::now).toStdWString());
	}

	SetImageSrc(toastXml, userpicPathWide);

	const auto nodeList = toastXml.GetElementsByTagName(L"text");
	if (nodeList.Length() < (withSubtitle ? 3U : 2U)) {
		return false;
	}

	SetNodeValueString(toastXml, nodeList.Item(0), title.toStdWString());
	if (withSubtitle) {
		SetNodeValueString(
			toastXml,
			nodeList.Item(1),
			subtitle.toStdWString());
	}
	SetNodeValueString(
		toastXml,
		nodeList.Item(withSubtitle ? 2 : 1),
		msg.toStdWString());

	const auto weak = std::weak_ptr(_guarded);
	const auto performOnMainQueue = [=](FnMut<void(Manager *manager)> task) {
		crl::on_main(weak, [=, task = std::move(task)]() mutable {
			task(*weak.lock());
		});
	};

	auto toast = ToastNotification(toastXml);
	const auto token1 = toast.Activated([=](
			const ToastNotification &sender,
			const winrt::Windows::Foundation::IInspectable &object) {
		auto activation = ToastActivation();
		const auto string = &ToastActivation::String;
		if (const auto args = object.try_as<ToastActivatedEventArgs>()) {
			activation.args = string(args.Arguments().c_str());
			const auto args2 = args.try_as<IToastActivatedEventArgs2>();
			if (!args2 && activation.args.startsWith("action=reply&")) {
				LOG(("WinRT Error: "
					"FastReply without IToastActivatedEventArgs2 support."));
				return;
			}
			const auto input = args2 ? args2.UserInput() : nullptr;
			const auto reply = input
				? input.TryLookup(L"fastReply")
				: nullptr;
			const auto data = reply
				? reply.try_as<IReference<winrt::hstring>>()
				: nullptr;
			if (data) {
				activation.input.push_back({
					.key = u"fastReply"_q,
					.value = string(data.GetString().c_str()),
				});
			}
		} else {
			activation.args = "action=open&" + idString;
		}
		crl::on_main([=, activation = std::move(activation)]() mutable {
			if (const auto strong = weak.lock()) {
				(*strong)->handleActivation(activation);
			}
		});
	});
	const auto token2 = toast.Dismissed([=](
			const ToastNotification &sender,
			const ToastDismissedEventArgs &args) {
		const auto reason = args.Reason();
		switch (reason) {
		case ToastDismissalReason::ApplicationHidden:
		case ToastDismissalReason::TimedOut: // Went to Action Center.
			break;
		case ToastDismissalReason::UserCanceled:
		default:
			performOnMainQueue([notificationId](Manager *manager) {
				manager->clearNotification(notificationId);
			});
			break;
		}
	});
	const auto token3 = toast.Failed([=](
			const ToastNotification &sender,
			const ToastFailedEventArgs &args) {
		performOnMainQueue([notificationId](Manager *manager) {
			manager->clearNotification(notificationId);
		});
	});

	auto i = _notifications.find(key);
	if (i != _notifications.cend()) {
		auto j = i->second.find(msgId);
		if (j != i->second.end()) {
			const auto existing = j->second;
			i->second.erase(j);
			tryHide(existing);
			i = _notifications.find(key);
		}
	}
	if (i == _notifications.cend()) {
		i = _notifications.emplace(
			key,
			base::flat_map<MsgId, ToastNotification>()).first;
	}
	if (!base::WinRT::Try([&] { _notifier.Show(toast); })) {
		i = _notifications.find(key);
		if (i != _notifications.cend() && i->second.empty()) {
			_notifications.erase(i);
		}
		return false;
	}
	i->second.emplace(msgId, toast);
	return true;
}

void Manager::Private::tryHide(const ToastNotification &notification) {
	base::WinRT::Try([&] {
		_notifier.Hide(notification);
	});
}

Manager::Manager(Window::Notifications::System *system)
: NativeManager(system)
, _private(std::make_unique<Private>(this)) {
}

bool Manager::init() {
	return _private->init();
}

void Manager::clearNotification(NotificationId id) {
	_private->clearNotification(id);
}

void Manager::handleActivation(const ToastActivation &activation) {
	_private->handleActivation(activation);
}

Manager::~Manager() = default;

void Manager::doShowNativeNotification(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		Ui::PeerUserpicView &userpicView,
		MsgId msgId,
		const QString &title,
		const QString &subtitle,
		const QString &msg,
		DisplayOptions options) {
	_private->showNotification(
		peer,
		topicRootId,
		userpicView,
		msgId,
		title,
		subtitle,
		msg,
		options);
}

void Manager::doClearAllFast() {
	_private->clearAll();
}

void Manager::doClearFromItem(not_null<HistoryItem*> item) {
	_private->clearFromItem(item);
}

void Manager::doClearFromTopic(not_null<Data::ForumTopic*> topic) {
	_private->clearFromTopic(topic);
}

void Manager::doClearFromHistory(not_null<History*> history) {
	_private->clearFromHistory(history);
}

void Manager::doClearFromSession(not_null<Main::Session*> session) {
	_private->clearFromSession(session);
}

void Manager::onBeforeNotificationActivated(NotificationId id) {
	_private->beforeNotificationActivated(id);
}

void Manager::onAfterNotificationActivated(
		NotificationId id,
		not_null<Window::SessionController*> window) {
	_private->afterNotificationActivated(id, window);
}

bool Manager::doSkipToast() const {
	return false;
}

void Manager::doMaybePlaySound(Fn<void()> playSound) {
	const auto skip = SkipSoundForCustom()
		|| QuietHoursEnabled
		|| FocusAssistBlocks;
	if (!skip) {
		playSound();
	}
}

void Manager::doMaybeFlashBounce(Fn<void()> flashBounce) {
	const auto skip = SkipFlashBounceForCustom()
		|| QuietHoursEnabled
		|| FocusAssistBlocks;
	if (!skip) {
		flashBounce();
	}
}
#endif // !__MINGW32__

} // namespace Notifications
} // namespace Platform
