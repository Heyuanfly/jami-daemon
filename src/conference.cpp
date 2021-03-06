/*
 *  Copyright (C) 2004-2022 Savoir-faire Linux Inc.
 *
 *  Author: Alexandre Savard <alexandre.savard@savoirfairelinux.com>
 *  Author: Guillaume Roguez <guillaume.roguez@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 */

#include <regex>
#include <sstream>

#include "conference.h"
#include "manager.h"
#include "audio/audiolayer.h"
#include "jamidht/jamiaccount.h"
#include "string_utils.h"
#include "sip/siptransport.h"

#include "client/videomanager.h"
#ifdef ENABLE_VIDEO
#include "call.h"
#include "video/video_input.h"
#include "video/video_mixer.h"
#endif

#ifdef ENABLE_PLUGIN
#include "plugin/jamipluginmanager.h"
#endif

#include "call_factory.h"

#include "logger.h"
#include "jami/media_const.h"
#include "audio/ringbufferpool.h"
#include "sip/sipcall.h"

#include <opendht/thread_pool.h>

using namespace std::literals;

namespace jami {

Conference::Conference(const std::shared_ptr<Account>& account)
    : id_(Manager::instance().callFactory.getNewCallID())
    , account_(account)
#ifdef ENABLE_VIDEO
    , videoEnabled_(account->isVideoEnabled())
#endif
{
    /** NOTE:
     *
     *** Handling mute state of the local host.
     *
     * When a call is added to a conference, the media source of the
     * call is set to the audio/video mixers output, and the host media
     * source (e.g. camera), is added as a source for the mixer.
     * Note that, by design, the mixers are never muted, but the mixer
     * can produce audio/video frames with no content (silence or black
     * video frames) if all the participants are muted.
     *
     * The mute state of the local host is set as follows:
     *
     * 1. If the video is disabled, the mute state is irrelevant.
     * 2. If the local is not attached, the mute state is irrelevant.
     * 3. When the conference is created from existing calls:
     *  the mute state is set to true if the local mute state of
     *  all participating calls are true.
     * 4. Attaching the local host to an existing conference:
     *  the audio and video is set to the default capture device
     *  (microphone and/or camera), and set to un-muted state.
     */

    JAMI_INFO("Create new conference %s", id_.c_str());
    setLocalHostDefaultMediaSource();

#ifdef ENABLE_VIDEO
    // We are done if the video is disabled.
    if (not videoEnabled_)
        return;

    videoMixer_ = std::make_shared<video::VideoMixer>(id_, hostVideoSource_.sourceUri_);
    videoMixer_->setOnSourcesUpdated([this](std::vector<video::SourceInfo>&& infos) {
        runOnMainThread([w = weak(), infos = std::move(infos)] {
            auto shared = w.lock();
            if (!shared)
                return;
            auto acc = std::dynamic_pointer_cast<JamiAccount>(shared->account_.lock());
            if (!acc)
                return;
            ConfInfo newInfo;
            auto hostAdded = false;
            // Handle participants showing their video
            std::unique_lock<std::mutex> lk(shared->videoToCallMtx_);
            for (const auto& info : infos) {
                std::string uri {};
                std::string deviceId {};
                auto it = shared->videoToCall_.find(info.source);
                if (it == shared->videoToCall_.end())
                    it = shared->videoToCall_.emplace_hint(it, info.source, std::string());
                bool isLocalMuted = false;
                // If not local
                if (!it->second.empty()) {
                    // Retrieve calls participants
                    // TODO: this is a first version, we assume that the peer is not
                    // a master of a conference and there is only one remote
                    // In the future, we should retrieve confInfo from the call
                    // To merge layouts informations
                    if (auto call = std::dynamic_pointer_cast<SIPCall>(getCall(it->second))) {
                        uri = call->getPeerNumber();
                        isLocalMuted = call->isPeerMuted();
                        if (auto* transport = call->getTransport())
                            deviceId = transport->deviceId();
                    }
                }
                auto active = false;
                if (auto videoMixer = shared->videoMixer_)
                    active = info.source == videoMixer->getActiveParticipant();
                std::string_view peerId = string_remove_suffix(uri, '@');
                auto isModerator = shared->isModerator(peerId);
                if (uri.empty()) {
                    hostAdded = true;
                    peerId = "host"sv;
                    deviceId = acc->currentDeviceId();
                    isLocalMuted = shared->isMediaSourceMuted(MediaType::MEDIA_AUDIO);
                }
                auto isHandRaised = shared->isHandRaised(peerId);
                auto isModeratorMuted = shared->isMuted(peerId);
                auto sinkId = shared->getConfId() + peerId;
                newInfo.emplace_back(ParticipantInfo {std::move(uri),
                                                      deviceId,
                                                      std::move(sinkId),
                                                      active,
                                                      info.x,
                                                      info.y,
                                                      info.w,
                                                      info.h,
                                                      !info.hasVideo,
                                                      isLocalMuted,
                                                      isModeratorMuted,
                                                      isModerator,
                                                      isHandRaised});
            }
            if (auto videoMixer = shared->videoMixer_) {
                newInfo.h = videoMixer->getHeight();
                newInfo.w = videoMixer->getWidth();
            }
            lk.unlock();
            if (!hostAdded) {
                ParticipantInfo pi;
                pi.videoMuted = true;
                pi.audioLocalMuted = shared->isMediaSourceMuted(MediaType::MEDIA_AUDIO);
                pi.isModerator = true;
                newInfo.emplace_back(pi);
            }

            shared->updateConferenceInfo(std::move(newInfo));
        });
    });
#endif
}

Conference::~Conference()
{
    JAMI_INFO("Destroying conference %s", id_.c_str());

#ifdef ENABLE_VIDEO
    foreachCall([&](auto call) {
        call->exitConference();
        // Reset distant callInfo
        call->resetConfInfo();
        // Trigger the SIP negotiation to update the resolution for the remaining call
        // ideally this sould be done without renegotiation
        call->switchInput(
            Manager::instance().getVideoManager().videoDeviceMonitor.getMRLForDefaultDevice());

        // Continue the recording for the call if the conference was recorded
        if (isRecording()) {
            JAMI_DBG("Stop recording for conf %s", getConfId().c_str());
            toggleRecording();
            if (not call->isRecording()) {
                JAMI_DBG("Conference was recorded, start recording for conf %s",
                         call->getCallId().c_str());
                call->toggleRecording();
            }
        }
        // Notify that the remaining peer is still recording after conference
        if (call->isPeerRecording())
            call->peerRecording(true);
    });
    if (videoMixer_) {
        auto& sink = videoMixer_->getSink();
        for (auto it = confSinksMap_.begin(); it != confSinksMap_.end();) {
            sink->detach(it->second.get());
            it->second->stop();
            it = confSinksMap_.erase(it);
        }
    }
#endif // ENABLE_VIDEO
#ifdef ENABLE_PLUGIN
    {
        std::lock_guard<std::mutex> lk(avStreamsMtx_);
        jami::Manager::instance()
            .getJamiPluginManager()
            .getCallServicesManager()
            .clearCallHandlerMaps(getConfId());
        Manager::instance().getJamiPluginManager().getCallServicesManager().clearAVSubject(
            getConfId());
        confAVStreams.clear();
    }
#endif // ENABLE_PLUGIN
}

Conference::State
Conference::getState() const
{
    return confState_;
}

void
Conference::setState(State state)
{
    JAMI_DBG("[conf %s] Set state to [%s] (was [%s])",
             id_.c_str(),
             getStateStr(state),
             getStateStr());

    confState_ = state;
}

void
Conference::setLocalHostDefaultMediaSource()
{
    // Setup local audio source
    if (confState_ == State::ACTIVE_ATTACHED) {
        hostAudioSource_ = {MediaType::MEDIA_AUDIO, false, false, true, {}, "audio_0"};
        hostAudioSource_.sourceType_ = MediaSourceType::CAPTURE_DEVICE;
    } else {
        hostAudioSource_ = {};
    }

    JAMI_DBG("[conf %s] Setting local host audio source to [%s]",
             id_.c_str(),
             hostAudioSource_.toString().c_str());

#ifdef ENABLE_VIDEO
    if (isVideoEnabled()) {
        // Setup local video source
        if (confState_ == State::ACTIVE_ATTACHED) {
            hostVideoSource_
                = {MediaType::MEDIA_VIDEO,
                   false,
                   false,
                   true,
                   Manager::instance().getVideoManager().videoDeviceMonitor.getMRLForDefaultDevice(),
                   "video_0"};
            hostVideoSource_.sourceType_ = MediaSourceType::CAPTURE_DEVICE;
        } else {
            hostVideoSource_ = {};
        }
        JAMI_DBG("[conf %s] Setting local host video source to [%s]",
                 id_.c_str(),
                 hostVideoSource_.toString().c_str());
    }
#endif
}

#ifdef ENABLE_PLUGIN
void
Conference::createConfAVStreams()
{
    auto audioMap = [](const std::shared_ptr<jami::MediaFrame>& m) -> AVFrame* {
        return std::static_pointer_cast<AudioFrame>(m)->pointer();
    };

    // Preview and Received
    if ((audioMixer_ = jami::getAudioInput(getConfId()))) {
        auto audioSubject = std::make_shared<MediaStreamSubject>(audioMap);
        StreamData previewStreamData {getConfId(), false, StreamType::audio, getConfId()};
        createConfAVStream(previewStreamData, *audioMixer_, audioSubject);
        StreamData receivedStreamData {getConfId(), true, StreamType::audio, getConfId()};
        createConfAVStream(receivedStreamData, *audioMixer_, audioSubject);
    }

#ifdef ENABLE_VIDEO

    if (videoMixer_) {
        // Review
        auto receiveSubject = std::make_shared<MediaStreamSubject>(pluginVideoMap_);
        StreamData receiveStreamData {getConfId(), true, StreamType::video, getConfId()};
        createConfAVStream(receiveStreamData, *videoMixer_, receiveSubject);

        // Preview
        if (auto& videoPreview = videoMixer_->getVideoLocal()) {
            auto previewSubject = std::make_shared<MediaStreamSubject>(pluginVideoMap_);
            StreamData previewStreamData {getConfId(), false, StreamType::video, getConfId()};
            createConfAVStream(previewStreamData, *videoPreview, previewSubject);
        }
    }
#endif // ENABLE_VIDEO
}

void
Conference::createConfAVStream(const StreamData& StreamData,
                               AVMediaStream& streamSource,
                               const std::shared_ptr<MediaStreamSubject>& mediaStreamSubject,
                               bool force)
{
    std::lock_guard<std::mutex> lk(avStreamsMtx_);
    const std::string AVStreamId = StreamData.id + std::to_string(static_cast<int>(StreamData.type))
                                   + std::to_string(StreamData.direction);
    auto it = confAVStreams.find(AVStreamId);
    if (!force && it != confAVStreams.end())
        return;

    confAVStreams.erase(AVStreamId);
    confAVStreams[AVStreamId] = mediaStreamSubject;
    streamSource.attachPriorityObserver(mediaStreamSubject);
    jami::Manager::instance()
        .getJamiPluginManager()
        .getCallServicesManager()
        .createAVSubject(StreamData, mediaStreamSubject);
}
#endif // ENABLE_PLUGIN

void
Conference::setLocalHostMuteState(MediaType type, bool muted)
{
    if (type == MediaType::MEDIA_AUDIO) {
        hostAudioSource_.muted_ = muted;
#ifdef ENABLE_VIDEO
    } else if (type == MediaType::MEDIA_VIDEO) {
        hostVideoSource_.muted_ = muted;
#endif
    } else {
        JAMI_ERR("Unsupported media type");
    }
}

bool
Conference::isMediaSourceMuted(MediaType type) const
{
    if (getState() != State::ACTIVE_ATTACHED) {
        // Assume muted if not attached.
        return true;
    }

    if (type != MediaType::MEDIA_AUDIO and type != MediaType::MEDIA_VIDEO) {
        JAMI_ERR("Unsupported media type");
        return true;
    }

#ifdef ENABLE_VIDEO
    auto const& mediaAttr = type == MediaType::MEDIA_AUDIO ? hostAudioSource_ : hostVideoSource_;
#else
    auto const& mediaAttr = hostAudioSource_;
#endif
    if (mediaAttr.type_ == MediaType::MEDIA_NONE) {
        JAMI_WARN("The host source for %s is not set. The mute state is meaningless",
                  mediaAttr.mediaTypeToString(mediaAttr.type_));
        // Assume muted if the media is not present.
        return true;
    }

    return mediaAttr.muted_;
}

void
Conference::takeOverMediaSourceControl(const std::string& callId)
{
    auto call = getCall(callId);
    if (not call) {
        JAMI_ERR("No call matches participant %s", callId.c_str());
        return;
    }

    auto account = call->getAccount().lock();
    if (not account) {
        JAMI_ERR("No account detected for call %s", callId.c_str());
        return;
    }

    auto mediaList = call->getMediaAttributeList();

    std::vector<MediaType> mediaTypeList {MediaType::MEDIA_AUDIO, MediaType::MEDIA_VIDEO};

    for (auto mediaType : mediaTypeList) {
        // Try to find a media with a valid source type
        auto check = [mediaType](auto const& mediaAttr) {
            return (mediaAttr.type_ == mediaType and mediaAttr.sourceType_ != MediaSourceType::NONE);
        };

        auto iter = std::find_if(mediaList.begin(), mediaList.end(), check);

        if (iter == mediaList.end()) {
            // Nothing to do if the call does not have a stream with
            // the requested media.
            JAMI_DBG("[Call: %s] Does not have an active [%s] media source",
                     callId.c_str(),
                     MediaAttribute::mediaTypeToString(mediaType));
            continue;
        }

        if (getState() == State::ACTIVE_ATTACHED) {
            // To mute the local source, all the sources of the participating
            // calls must be muted. If it's the first participant, just use
            // its mute state.
            if (participants_.size() == 1) {
                setLocalHostMuteState(iter->type_, iter->muted_);
            } else {
                setLocalHostMuteState(iter->type_, iter->muted_ and isMediaSourceMuted(iter->type_));
            }
        }

        // Un-mute media in the call. The mute/un-mute state will be handled
        // by the conference/mixer from now on.
        iter->muted_ = false;
    }

    // Update the media states in the newly added call.
    call->requestMediaChange(MediaAttribute::mediaAttributesToMediaMaps(mediaList));

    // Notify the client
    for (auto mediaType : mediaTypeList) {
        if (mediaType == MediaType::MEDIA_AUDIO) {
            bool muted = isMediaSourceMuted(MediaType::MEDIA_AUDIO);
            JAMI_WARN("Take over [AUDIO] control from call %s - current local source state [%s]",
                      callId.c_str(),
                      muted ? "muted" : "un-muted");
            emitSignal<DRing::CallSignal::AudioMuted>(id_, muted);
        } else {
            bool muted = isMediaSourceMuted(MediaType::MEDIA_VIDEO);
            JAMI_WARN("Take over [VIDEO] control from call %s - current local source state [%s]",
                      callId.c_str(),
                      muted ? "muted" : "un-muted");
            emitSignal<DRing::CallSignal::VideoMuted>(id_, muted);
        }
    }
}

bool
Conference::requestMediaChange(const std::vector<DRing::MediaMap>& mediaList)
{
    if (getState() != State::ACTIVE_ATTACHED) {
        JAMI_ERR("[conf %s] Request media change can be performed only in attached mode",
                 getConfId().c_str());
        return false;
    }

    JAMI_DBG("[conf %s] Request media change", getConfId().c_str());

    auto mediaAttrList = MediaAttribute::buildMediaAttributesList(mediaList, false);

    for (auto const& mediaAttr : mediaAttrList) {
        JAMI_DBG("[conf %s] New requested media: %s",
                 getConfId().c_str(),
                 mediaAttr.toString(true).c_str());
    }

    // NOTE:
    // The current design support only one stream per media type. The
    // request will be ignored if this condition is not respected.
    for (auto mediaType : {MediaType::MEDIA_AUDIO, MediaType::MEDIA_VIDEO}) {
        auto count = std::count_if(mediaAttrList.begin(),
                                   mediaAttrList.end(),
                                   [&mediaType](auto const& attr) {
                                       return attr.type_ == mediaType;
                                   });

        if (count > 1) {
            JAMI_ERR("[conf %s] Cant handle more than 1 stream per media type (found %lu)",
                     getConfId().c_str(),
                     count);
            return false;
        }
    }

    for (auto const& mediaAttr : mediaAttrList) {
#ifdef ENABLE_VIDEO
        auto& mediaSource = mediaAttr.type_ == MediaType::MEDIA_AUDIO ? hostAudioSource_
                                                                      : hostVideoSource_;
#else
        auto& mediaSource = hostAudioSource_;
#endif
        if (not mediaAttr.sourceUri_.empty() and mediaSource.sourceUri_ != mediaAttr.sourceUri_) {
            // For now, only video source URI can be changed by the client,
            // so it's an error if we get here and the type is not video.
            if (mediaAttr.type_ != MediaType::MEDIA_VIDEO) {
                JAMI_ERR("[conf %s] Media source can be changed only for video!",
                         getConfId().c_str());
                return false;
            }

            mediaSource.sourceUri_ = mediaAttr.sourceUri_;
            mediaSource.sourceType_ = mediaAttr.sourceType_;

            if (mediaSource.muted_ != mediaAttr.muted_) {
                // If the current media source is muted, just call un-mute, it
                // will set the new source as input.
                muteLocalHost(mediaAttr.muted_,
                              mediaAttr.type_ == MediaType::MEDIA_AUDIO
                                  ? DRing::Media::Details::MEDIA_TYPE_AUDIO
                                  : DRing::Media::Details::MEDIA_TYPE_VIDEO);
            } else {
                switchInput(mediaSource.sourceUri_);
            }
        }

        // Update the mute state if changed.
        if (mediaSource.muted_ != mediaAttr.muted_) {
            muteLocalHost(mediaAttr.muted_,
                          mediaAttr.type_ == MediaType::MEDIA_AUDIO
                              ? DRing::Media::Details::MEDIA_TYPE_AUDIO
                              : DRing::Media::Details::MEDIA_TYPE_VIDEO);
        }
    }

    return true;
}

void
Conference::handleMediaChangeRequest(const std::shared_ptr<Call>& call,
                                     const std::vector<DRing::MediaMap>& remoteMediaList)
{
    JAMI_DBG("Conf [%s] Answer to media change request", getConfId().c_str());

#ifdef ENABLE_VIDEO
    // If the new media list has video, remove existing dummy
    // video sessions if any.
    if (MediaAttribute::hasMediaType(MediaAttribute::buildMediaAttributesList(remoteMediaList,
                                                                              false),
                                     MediaType::MEDIA_VIDEO)) {
        call->removeDummyVideoRtpSessions();
    }
#endif

    // Check if we need to update the mixer.
    // We need to check before the media is changed.
    auto updateMixer = call->checkMediaChangeRequest(remoteMediaList);

    // NOTE:
    // Since this is a conference, newly added media will be also
    // accepted.
    // This also means that if original call was an audio-only call,
    // the local camera will be enabled, unless the video is disabled
    // in the account settings.

    std::vector<DRing::MediaMap> newMediaList;
    newMediaList.reserve(remoteMediaList.size());
    for (auto const& media : call->getMediaAttributeList()) {
        newMediaList.emplace_back(MediaAttribute::toMediaMap(media));
    }

    if (remoteMediaList.size() > newMediaList.size()) {
        for (auto idx = newMediaList.size(); idx < remoteMediaList.size(); idx++) {
            newMediaList.emplace_back(remoteMediaList[idx]);
        }
    }

    call->answerMediaChangeRequest(newMediaList);
    call->enterConference(shared_from_this());

    if (updateMixer and getState() == Conference::State::ACTIVE_ATTACHED) {
        detachLocalParticipant();
        attachLocalParticipant();
    }
}

void
Conference::addParticipant(const std::string& participant_id)
{
    JAMI_DBG("Adding call %s to conference %s", participant_id.c_str(), id_.c_str());

    {
        std::lock_guard<std::mutex> lk(participantsMtx_);
        if (!participants_.insert(participant_id).second)
            return;
    }

    // Check if participant was muted before conference
    if (auto call = getCall(participant_id)) {
        if (call->isPeerMuted()) {
            participantsMuted_.emplace(string_remove_suffix(call->getPeerNumber(), '@'));
        }

        // NOTE:
        // When a call joins a conference, the media source of the call
        // will be set to the output of the conference mixer.
        takeOverMediaSourceControl(participant_id);
    }

    if (auto call = getCall(participant_id)) {
        auto w = call->getAccount();
        auto account = w.lock();
        if (account) {
            // Add defined moderators for the account link to the call
            for (const auto& mod : account->getDefaultModerators()) {
                moderators_.emplace(mod);
            }

            // Check for localModeratorsEnabled preference
            if (account->isLocalModeratorsEnabled() && not localModAdded_) {
                auto accounts = jami::Manager::instance().getAllAccounts<JamiAccount>();
                for (const auto& account : accounts) {
                    moderators_.emplace(account->getUsername());
                }
                localModAdded_ = true;
            }

            // Check for allModeratorEnabled preference
            if (account->isAllModerators()) {
                moderators_.emplace(string_remove_suffix(call->getPeerNumber(), '@'));
            }
        }
    }
#ifdef ENABLE_VIDEO
    if (auto call = getCall(participant_id)) {
        // In conference, all participants need to have video session
        // (with a sink) in order to display the participant info in
        // the layout. So, if a participant joins with an audio only
        // call, a dummy video stream is added to the call.
        auto mediaList = call->getMediaAttributeList();
        if (not MediaAttribute::hasMediaType(mediaList, MediaType::MEDIA_VIDEO)) {
            call->addDummyVideoRtpSession();
        }
        call->enterConference(shared_from_this());
        // Continue the recording for the conference if one participant was recording
        if (call->isRecording()) {
            JAMI_DBG("Stop recording for call %s", call->getCallId().c_str());
            call->toggleRecording();
            if (not this->isRecording()) {
                JAMI_DBG("One participant was recording, start recording for conference %s",
                         getConfId().c_str());
                this->toggleRecording();
            }
        }
    } else
        JAMI_ERR("no call associate to participant %s", participant_id.c_str());
#endif // ENABLE_VIDEO
#ifdef ENABLE_PLUGIN
    createConfAVStreams();
#endif
}

void
Conference::setActiveParticipant(const std::string& participant_id)
{
#ifdef ENABLE_VIDEO
    if (!videoMixer_)
        return;
    if (isHost(participant_id)) {
        videoMixer_->setActiveHost();
        return;
    }
    if (auto call = getCallFromPeerID(participant_id)) {
        if (auto videoRecv = call->getReceiveVideoFrameActiveWriter())
            videoMixer_->setActiveParticipant(videoRecv.get());
        return;
    }

    auto remoteHost = findHostforRemoteParticipant(participant_id);
    if (not remoteHost.empty()) {
        // This logic will be handled client side
        JAMI_WARN("Change remote layout is not supported");
        return;
    }
    // Unset active participant by default
    videoMixer_->setActiveParticipant(nullptr);
#endif
}

void
Conference::setLayout(int layout)
{
#ifdef ENABLE_VIDEO
    switch (layout) {
    case 0:
        videoMixer_->setVideoLayout(video::Layout::GRID);
        // The layout shouldn't have an active participant
        if (videoMixer_->getActiveParticipant())
            videoMixer_->setActiveParticipant(nullptr);
        break;
    case 1:
        videoMixer_->setVideoLayout(video::Layout::ONE_BIG_WITH_SMALL);
        break;
    case 2:
        videoMixer_->setVideoLayout(video::Layout::ONE_BIG);
        break;
    default:
        break;
    }
#endif
}

std::vector<std::map<std::string, std::string>>
ConfInfo::toVectorMapStringString() const
{
    std::vector<std::map<std::string, std::string>> infos;
    infos.reserve(size());
    for (const auto& info : *this)
        infos.emplace_back(info.toMap());
    return infos;
}

std::string
ConfInfo::toString() const
{
    Json::Value val = {};
    for (const auto& info : *this) {
        val["p"].append(info.toJson());
    }
    val["w"] = w;
    val["h"] = h;
    return Json::writeString(Json::StreamWriterBuilder {}, val);
}

void
Conference::sendConferenceInfos()
{
    // Inform calls that the layout has changed
    foreachCall([&](auto call) {
        // Produce specific JSON for each participant (2 separate accounts can host ...
        // a conference on a same device, the conference is not link to one account).
        auto w = call->getAccount();
        auto account = w.lock();
        if (!account)
            return;

        dht::ThreadPool::io().run(
            [call,
             confInfo = getConfInfoHostUri(account->getUsername() + "@ring.dht",
                                           call->getPeerNumber())] {
                call->sendConfInfo(confInfo.toString());
            });
    });

    auto confInfo = getConfInfoHostUri("", "");
#ifdef ENABLE_VIDEO
    createSinks(confInfo);
#endif

    // Inform client that layout has changed
    jami::emitSignal<DRing::CallSignal::OnConferenceInfosUpdated>(id_,
                                                                  confInfo.toVectorMapStringString());
}

#ifdef ENABLE_VIDEO
void
Conference::createSinks(const ConfInfo& infos)
{
    std::lock_guard<std::mutex> lk(sinksMtx_);
    if (!videoMixer_)
        return;
    auto& sink = videoMixer_->getSink();
    Manager::instance().createSinkClients(getConfId(),
                                          infos,
                                          std::static_pointer_cast<video::VideoFrameActiveWriter>(
                                              sink),
                                          confSinksMap_);
}

void
Conference::attachVideo(Observable<std::shared_ptr<MediaFrame>>* frame, const std::string& callId)
{
    JAMI_DBG("[conf:%s] attaching video of call %s", id_.c_str(), callId.c_str());
    std::lock_guard<std::mutex> lk(videoToCallMtx_);
    videoToCall_.emplace(frame, callId);
    frame->attach(videoMixer_.get());
}

void
Conference::detachVideo(Observable<std::shared_ptr<MediaFrame>>* frame)
{
    std::lock_guard<std::mutex> lk(videoToCallMtx_);
    auto it = videoToCall_.find(frame);
    if (it != videoToCall_.end()) {
        JAMI_DBG("[conf:%s] detaching video of call %s", id_.c_str(), it->second.c_str());
        it->first->detach(videoMixer_.get());
        videoToCall_.erase(it);
    }
}
#endif

void
Conference::removeParticipant(const std::string& participant_id)
{
    {
        std::lock_guard<std::mutex> lk(participantsMtx_);
        if (!participants_.erase(participant_id))
            return;
    }
    if (auto call = getCall(participant_id)) {
        participantsMuted_.erase(std::string(string_remove_suffix(call->getPeerNumber(), '@')));
        handsRaised_.erase(std::string(string_remove_suffix(call->getPeerNumber(), '@')));
#ifdef ENABLE_VIDEO
        call->exitConference();
        if (call->isPeerRecording())
            call->peerRecording(false);
#endif // ENABLE_VIDEO
    }
}

void
Conference::attachLocalParticipant()
{
    JAMI_INFO("Attach local participant to conference %s", id_.c_str());

    if (getState() == State::ACTIVE_DETACHED) {
        setState(State::ACTIVE_ATTACHED);
        setLocalHostDefaultMediaSource();

        auto& rbPool = Manager::instance().getRingBufferPool();
        for (const auto& participant : getParticipantList()) {
            if (auto call = Manager::instance().getCallFromCallID(participant)) {
                if (isMuted(string_remove_suffix(call->getPeerNumber(), '@')))
                    rbPool.bindHalfDuplexOut(participant, RingBufferPool::DEFAULT_ID);
                else
                    rbPool.bindCallID(participant, RingBufferPool::DEFAULT_ID);
                rbPool.flush(participant);
            }

            // Reset ringbuffer's readpointers
            rbPool.flush(participant);
        }
        rbPool.flush(RingBufferPool::DEFAULT_ID);

#ifdef ENABLE_VIDEO
        if (videoMixer_) {
            videoMixer_->switchInput(hostVideoSource_.sourceUri_);
            if (not mediaSecondaryInput_.empty())
                videoMixer_->switchSecondaryInput(mediaSecondaryInput_);
        }
#endif
    } else {
        JAMI_WARN(
            "Invalid conference state in attach participant: current \"%s\" - expected \"%s\"",
            getStateStr(),
            "ACTIVE_DETACHED");
    }
}

void
Conference::detachLocalParticipant()
{
    JAMI_INFO("Detach local participant from conference %s", id_.c_str());

    if (getState() == State::ACTIVE_ATTACHED) {
        foreachCall([&](auto call) {
            Manager::instance().getRingBufferPool().unBindCallID(call->getCallId(),
                                                                 RingBufferPool::DEFAULT_ID);
        });

        // Reset local audio source
        hostAudioSource_ = {};

#ifdef ENABLE_VIDEO
        if (videoMixer_)
            videoMixer_->stopInput();

        // Reset local video source
        hostVideoSource_ = {};
#endif
        setState(State::ACTIVE_DETACHED);
    } else {
        JAMI_WARN(
            "Invalid conference state in detach participant: current \"%s\" - expected \"%s\"",
            getStateStr(),
            "ACTIVE_ATTACHED");
    }

    setLocalHostDefaultMediaSource();
}

void
Conference::bindParticipant(const std::string& participant_id)
{
    JAMI_INFO("Bind participant %s to conference %s", participant_id.c_str(), id_.c_str());

    auto& rbPool = Manager::instance().getRingBufferPool();

    for (const auto& item : getParticipantList()) {
        if (participant_id != item) {
            // Do not attach muted participants
            if (auto call = Manager::instance().getCallFromCallID(item)) {
                if (isMuted(string_remove_suffix(call->getPeerNumber(), '@')))
                    rbPool.bindHalfDuplexOut(item, participant_id);
                else
                    rbPool.bindCallID(participant_id, item);
            }
        }
        rbPool.flush(item);
    }

    // Bind local participant to other participants only if the
    // local is attached to the conference.
    if (getState() == State::ACTIVE_ATTACHED) {
        if (isMediaSourceMuted(MediaType::MEDIA_AUDIO))
            rbPool.bindHalfDuplexOut(RingBufferPool::DEFAULT_ID, participant_id);
        else
            rbPool.bindCallID(participant_id, RingBufferPool::DEFAULT_ID);
        rbPool.flush(RingBufferPool::DEFAULT_ID);
    }
}

void
Conference::unbindParticipant(const std::string& participant_id)
{
    JAMI_INFO("Unbind participant %s from conference %s", participant_id.c_str(), id_.c_str());
    Manager::instance().getRingBufferPool().unBindAllHalfDuplexOut(participant_id);
}

void
Conference::bindHost()
{
    JAMI_INFO("Bind host to conference %s", id_.c_str());

    auto& rbPool = Manager::instance().getRingBufferPool();

    for (const auto& item : getParticipantList()) {
        if (auto call = Manager::instance().getCallFromCallID(item)) {
            if (isMuted(string_remove_suffix(call->getPeerNumber(), '@')))
                continue;
            rbPool.bindCallID(item, RingBufferPool::DEFAULT_ID);
            rbPool.flush(RingBufferPool::DEFAULT_ID);
        }
    }
}

void
Conference::unbindHost()
{
    JAMI_INFO("Unbind host from conference %s", id_.c_str());
    Manager::instance().getRingBufferPool().unBindAllHalfDuplexOut(RingBufferPool::DEFAULT_ID);
}

ParticipantSet
Conference::getParticipantList() const
{
    std::lock_guard<std::mutex> lk(participantsMtx_);
    return participants_;
}

bool
Conference::toggleRecording()
{
    bool newState = not isRecording();
    if (newState)
        initRecorder(recorder_);
    else
        deinitRecorder(recorder_);

    // Notify each participant
    foreachCall([&](auto call) { call->updateRecState(newState); });

    return Recordable::toggleRecording();
}

std::string
Conference::getAccountId() const
{
    if (auto account = getAccount())
        return account->getAccountID();
    return {};
}

void
Conference::switchInput(const std::string& input)
{
#ifdef ENABLE_VIDEO
    JAMI_DBG("[Conf:%s] Setting video input to %s", id_.c_str(), input.c_str());

    hostVideoSource_.sourceUri_ = input;

    // Done if the video is disabled
    if (not isVideoEnabled())
        return;

    if (auto mixer = videoMixer_) {
        mixer->switchInput(input);
#ifdef ENABLE_PLUGIN
        // Preview
        if (auto& videoPreview = mixer->getVideoLocal()) {
            auto previewSubject = std::make_shared<MediaStreamSubject>(pluginVideoMap_);
            StreamData previewStreamData {getConfId(), false, StreamType::video, getConfId()};
            createConfAVStream(previewStreamData, *videoPreview, previewSubject, true);
        }
#endif
    }
#endif
}

void
Conference::switchSecondaryInput(const std::string& input)
{
#ifdef ENABLE_VIDEO
    mediaSecondaryInput_ = input;
    if (videoMixer_) {
        videoMixer_->switchSecondaryInput(input);
    }
#endif
}

bool
Conference::isVideoEnabled() const
{
    if (auto shared = account_.lock())
        return shared->isVideoEnabled();
    return false;
}

#ifdef ENABLE_VIDEO
std::shared_ptr<video::VideoMixer>
Conference::getVideoMixer()
{
    return videoMixer_;
}
#endif

void
Conference::initRecorder(std::shared_ptr<MediaRecorder>& rec)
{
#ifdef ENABLE_VIDEO
    // Video
    if (videoMixer_) {
        if (auto ob = rec->addStream(videoMixer_->getStream("v:mixer"))) {
            videoMixer_->attach(ob);
        }
    }
#endif

    // Audio
    // Create ghost participant for ringbufferpool
    auto& rbPool = Manager::instance().getRingBufferPool();
    ghostRingBuffer_ = rbPool.createRingBuffer(getConfId());

    // Bind it to ringbufferpool in order to get the all mixed frames
    bindParticipant(getConfId());

    // Add stream to recorder
    audioMixer_ = jami::getAudioInput(getConfId());
    if (auto ob = rec->addStream(audioMixer_->getInfo("a:mixer"))) {
        audioMixer_->attach(ob);
    }
}

void
Conference::deinitRecorder(std::shared_ptr<MediaRecorder>& rec)
{
#ifdef ENABLE_VIDEO
    // Video
    if (videoMixer_) {
        if (auto ob = rec->getStream("v:mixer")) {
            videoMixer_->detach(ob);
        }
    }
#endif

    // Audio
    if (auto ob = rec->getStream("a:mixer"))
        audioMixer_->detach(ob);
    audioMixer_.reset();
    Manager::instance().getRingBufferPool().unBindAll(getConfId());
    ghostRingBuffer_.reset();
}

void
Conference::onConfOrder(const std::string& callId, const std::string& confOrder)
{
    // Check if the peer is a master
    if (auto call = Manager::instance().getCallFromCallID(callId)) {
        auto peerID = string_remove_suffix(call->getPeerNumber(), '@');

        std::string err;
        Json::Value root;
        Json::CharReaderBuilder rbuilder;
        auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());
        if (!reader->parse(confOrder.c_str(), confOrder.c_str() + confOrder.size(), &root, &err)) {
            JAMI_WARN("Couldn't parse conference order from %.*s",
                      (int) peerID.size(),
                      peerID.data());
            return;
        }

        if (root.isMember("handRaised")) {
            auto state = root["handState"].asString() == "true";
            if (peerID == root["handRaised"].asString()) {
                // In this case, the user want to change their state
                setHandRaised(root["handRaised"].asString(), state);
            } else if (!state && isModerator(peerID)) {
                // In this case a moderator can lower the hand
                setHandRaised(root["handRaised"].asString(), state);
            }
        }

        if (!isModerator(peerID)) {
            JAMI_WARN("Received conference order from a non master (%.*s)",
                      (int) peerID.size(),
                      peerID.data());
            return;
        }
        if (isVideoEnabled() and root.isMember("layout")) {
            setLayout(root["layout"].asUInt());
        }
        if (root.isMember("activeParticipant")) {
            setActiveParticipant(root["activeParticipant"].asString());
        }
        if (root.isMember("muteParticipant") and root.isMember("muteState")) {
            muteParticipant(root["muteParticipant"].asString(),
                            root["muteState"].asString() == "true");
        }
        if (root.isMember("hangupParticipant")) {
            hangupParticipant(root["hangupParticipant"].asString());
        }
    }
}

std::shared_ptr<Call>
Conference::getCall(const std::string& callId)
{
    return Manager::instance().callFactory.getCall(callId);
}

bool
Conference::isModerator(std::string_view uri) const
{
    return moderators_.find(uri) != moderators_.end() or isHost(uri);
}

bool
Conference::isHandRaised(std::string_view uri) const
{
    return isHost(uri) ? handsRaised_.find("host"sv) != handsRaised_.end()
                       : handsRaised_.find(uri) != handsRaised_.end();
}

void
Conference::setHandRaised(const std::string& participant_id, const bool& state)
{
    if (isHost(participant_id)) {
        auto isPeerRequiringAttention = isHandRaised("host"sv);
        if (state and not isPeerRequiringAttention) {
            JAMI_DBG("Raise host hand");
            handsRaised_.emplace("host"sv);
            updateHandsRaised();
        } else if (not state and isPeerRequiringAttention) {
            JAMI_DBG("Lower host hand");
            handsRaised_.erase("host");
            updateHandsRaised();
        }
        return;
    } else {
        for (const auto& p : getParticipantList()) {
            if (auto call = getCall(p)) {
                auto isPeerRequiringAttention = isHandRaised(participant_id);
                if (participant_id == string_remove_suffix(call->getPeerNumber(), '@')) {
                    if (state and not isPeerRequiringAttention) {
                        JAMI_DBG("Raise %s hand", participant_id.c_str());
                        handsRaised_.emplace(participant_id);
                        updateHandsRaised();
                    } else if (not state and isPeerRequiringAttention) {
                        JAMI_DBG("Remove %s raised hand", participant_id.c_str());
                        handsRaised_.erase(participant_id);
                        updateHandsRaised();
                    }
                    return;
                }
            }
        }
    }
    JAMI_WARN("Fail to raise %s hand (participant not found)", participant_id.c_str());
}

void
Conference::setModerator(const std::string& participant_id, const bool& state)
{
    for (const auto& p : getParticipantList()) {
        if (auto call = getCall(p)) {
            auto isPeerModerator = isModerator(participant_id);
            if (participant_id == string_remove_suffix(call->getPeerNumber(), '@')) {
                if (state and not isPeerModerator) {
                    JAMI_DBG("Add %s as moderator", participant_id.c_str());
                    moderators_.emplace(participant_id);
                    updateModerators();
                } else if (not state and isPeerModerator) {
                    JAMI_DBG("Remove %s as moderator", participant_id.c_str());
                    moderators_.erase(participant_id);
                    updateModerators();
                }
                return;
            }
        }
    }
    JAMI_WARN("Fail to set %s as moderator (participant not found)", participant_id.c_str());
}

void
Conference::updateModerators()
{
    std::lock_guard<std::mutex> lk(confInfoMutex_);
    for (auto& info : confInfo_) {
        info.isModerator = isModerator(string_remove_suffix(info.uri, '@'));
    }
    sendConferenceInfos();
}

void
Conference::updateHandsRaised()
{
    std::lock_guard<std::mutex> lk(confInfoMutex_);
    for (auto& info : confInfo_) {
        info.handRaised = isHandRaised(string_remove_suffix(info.uri, '@'));
    }
    sendConferenceInfos();
}

void
Conference::foreachCall(const std::function<void(const std::shared_ptr<Call>& call)>& cb)
{
    for (const auto& p : getParticipantList())
        if (auto call = getCall(p))
            cb(call);
}

bool
Conference::isMuted(std::string_view uri) const
{
    return participantsMuted_.find(uri) != participantsMuted_.end();
}

void
Conference::muteParticipant(const std::string& participant_id, const bool& state)
{
    // Prioritize remote mute, otherwise the mute info is lost during
    // the conference merge (we don't send back info to remoteHost,
    // cf. getConfInfoHostUri method)

    // Transfert remote participant mute
    auto remoteHost = findHostforRemoteParticipant(participant_id);
    if (not remoteHost.empty()) {
        if (auto call = getCallFromPeerID(string_remove_suffix(remoteHost, '@'))) {
            auto w = call->getAccount();
            auto account = w.lock();
            if (!account)
                return;
            Json::Value root;
            root["muteParticipant"] = participant_id;
            root["muteState"] = state ? TRUE_STR : FALSE_STR;
            call->sendConfOrder(root);
            return;
        }
    }

    // Moderator mute host
    if (isHost(participant_id)) {
        auto isHostMuted = isMuted("host"sv);
        if (state and not isHostMuted) {
            participantsMuted_.emplace("host"sv);
            if (not isMediaSourceMuted(MediaType::MEDIA_AUDIO)) {
                JAMI_DBG("Mute host");
                unbindHost();
            }
        } else if (not state and isHostMuted) {
            participantsMuted_.erase("host");
            if (not isMediaSourceMuted(MediaType::MEDIA_AUDIO)) {
                JAMI_DBG("Unmute host");
                bindHost();
            }
        }
        updateMuted();
        return;
    }

    // Mute participant
    if (auto call = getCallFromPeerID(participant_id)) {
        auto isPartMuted = isMuted(participant_id);
        if (state and not isPartMuted) {
            JAMI_DBG("Mute participant %.*s", (int) participant_id.size(), participant_id.data());
            participantsMuted_.emplace(std::string(participant_id));
            unbindParticipant(call->getCallId());
            updateMuted();
        } else if (not state and isPartMuted) {
            JAMI_DBG("Unmute participant %.*s", (int) participant_id.size(), participant_id.data());
            participantsMuted_.erase(std::string(participant_id));
            bindParticipant(call->getCallId());
            updateMuted();
        }
        return;
    }
}

void
Conference::updateMuted()
{
    std::lock_guard<std::mutex> lk(confInfoMutex_);
    for (auto& info : confInfo_) {
        auto peerID = string_remove_suffix(info.uri, '@');
        if (peerID.empty()) {
            peerID = "host"sv;
            info.audioModeratorMuted = isMuted(peerID);
            info.audioLocalMuted = isMediaSourceMuted(MediaType::MEDIA_AUDIO);
        } else {
            info.audioModeratorMuted = isMuted(peerID);
            if (auto call = getCallFromPeerID(peerID))
                info.audioLocalMuted = call->isPeerMuted();
        }
    }
    sendConferenceInfos();
}

ConfInfo
Conference::getConfInfoHostUri(std::string_view localHostURI, std::string_view destURI)
{
    ConfInfo newInfo = confInfo_;

    for (auto it = newInfo.begin(); it != newInfo.end();) {
        bool isRemoteHost = remoteHosts_.find(it->uri) != remoteHosts_.end();
        if (it->uri.empty() and not destURI.empty()) {
            // fill the empty uri with the local host URI, let void for local client
            it->uri = localHostURI;
        }
        if (isRemoteHost) {
            // Don't send back the ParticipantInfo for remote Host
            // For other than remote Host, the new info is in remoteHosts_
            it = newInfo.erase(it);
        } else {
            ++it;
        }
    }
    // Add remote Host info
    for (const auto& [hostUri, confInfo] : remoteHosts_) {
        // Add remote info for remote host destination
        // Example: ConfA, ConfB & ConfC
        // ConfA send ConfA and ConfB for ConfC
        // ConfA send ConfA and ConfC for ConfB
        // ...
        if (destURI != hostUri)
            newInfo.insert(newInfo.end(), confInfo.begin(), confInfo.end());
    }
    return newInfo;
}

bool
Conference::isHost(std::string_view uri) const
{
    if (uri.empty())
        return true;

    // Check if the URI is a local URI (AccountID) for at least one of the subcall
    // (a local URI can be in the call with another device)
    for (const auto& p : getParticipantList()) {
        if (auto call = getCall(p)) {
            if (auto account = call->getAccount().lock()) {
                if (account->getUsername() == uri)
                    return true;
            }
        }
    }
    return false;
}

void
Conference::updateConferenceInfo(ConfInfo confInfo)
{
    std::lock_guard<std::mutex> lk(confInfoMutex_);
    confInfo_ = std::move(confInfo);
    sendConferenceInfos();
}

void
Conference::hangupParticipant(const std::string& participant_id)
{
    if (isHost(participant_id)) {
        Manager::instance().detachLocalParticipant(shared_from_this());
        return;
    }

    if (auto call = getCallFromPeerID(participant_id)) {
        if (auto account = call->getAccount().lock()) {
            Manager::instance().hangupCall(account->getAccountID(), call->getCallId());
        }
        return;
    }

    // Transfert remote participant hangup
    auto remoteHost = findHostforRemoteParticipant(participant_id);
    if (remoteHost.empty()) {
        JAMI_WARN("Can't hangup %s, peer not found", participant_id.c_str());
        return;
    }
    if (auto call = getCallFromPeerID(string_remove_suffix(remoteHost, '@'))) {
        auto w = call->getAccount();
        auto account = w.lock();
        if (!account)
            return;

        Json::Value root;
        root["hangupParticipant"] = participant_id;
        call->sendConfOrder(root);
        return;
    }
}

void
Conference::muteLocalHost(bool is_muted, const std::string& mediaType)
{
    if (mediaType.compare(DRing::Media::Details::MEDIA_TYPE_AUDIO) == 0) {
        if (is_muted == isMediaSourceMuted(MediaType::MEDIA_AUDIO)) {
            JAMI_DBG("Local audio source already in [%s] state", is_muted ? "muted" : "un-muted");
            return;
        }

        auto isHostMuted = isMuted("host"sv);
        if (is_muted and not isMediaSourceMuted(MediaType::MEDIA_AUDIO) and not isHostMuted) {
            JAMI_DBG("Muting local audio source");
            unbindHost();
        } else if (not is_muted and isMediaSourceMuted(MediaType::MEDIA_AUDIO) and not isHostMuted) {
            JAMI_DBG("Un-muting local audio source");
            bindHost();
        }
        setLocalHostMuteState(MediaType::MEDIA_AUDIO, is_muted);
        updateMuted();
        emitSignal<DRing::CallSignal::AudioMuted>(id_, is_muted);
        return;
    } else if (mediaType.compare(DRing::Media::Details::MEDIA_TYPE_VIDEO) == 0) {
#ifdef ENABLE_VIDEO
        if (not isVideoEnabled()) {
            JAMI_ERR("Cant't mute, the video is disabled!");
            return;
        }

        if (is_muted == isMediaSourceMuted(MediaType::MEDIA_VIDEO)) {
            JAMI_DBG("Local video source already in [%s] state", is_muted ? "muted" : "un-muted");
            return;
        }
        setLocalHostMuteState(MediaType::MEDIA_VIDEO, is_muted);
        if (is_muted) {
            if (auto mixer = videoMixer_) {
                JAMI_DBG("Muting local video source");
                mixer->stopInput();
            }
        } else {
            if (auto mixer = videoMixer_) {
                JAMI_DBG("Un-muting local video source");
                switchInput(hostVideoSource_.sourceUri_);
            }
        }
        emitSignal<DRing::CallSignal::VideoMuted>(id_, is_muted);
        return;
#endif
    }
}

#ifdef ENABLE_VIDEO
void
Conference::resizeRemoteParticipants(ConfInfo& confInfo, std::string_view peerURI)
{
    int remoteFrameHeight = confInfo.h;
    int remoteFrameWidth = confInfo.w;

    if (remoteFrameHeight == 0 or remoteFrameWidth == 0) {
        // get the size of the remote frame from receiveThread
        // if the one from confInfo is empty
        if (auto call = std::dynamic_pointer_cast<SIPCall>(
                getCallFromPeerID(string_remove_suffix(peerURI, '@')))) {
            if (auto const& videoRtp = call->getVideoRtp()) {
                remoteFrameHeight = videoRtp->getVideoReceive()->getHeight();
                remoteFrameWidth = videoRtp->getVideoReceive()->getWidth();
            }
        }
    }

    if (remoteFrameHeight == 0 or remoteFrameWidth == 0) {
        JAMI_WARN("Remote frame size not found.");
        return;
    }

    // get the size of the local frame
    ParticipantInfo localCell;
    for (const auto& p : confInfo_) {
        if (p.uri == peerURI) {
            localCell = p;
            break;
        }
    }

    const float zoomX = (float) remoteFrameWidth / localCell.w;
    const float zoomY = (float) remoteFrameHeight / localCell.h;
    // Do the resize for each remote participant
    for (auto& remoteCell : confInfo) {
        remoteCell.x = remoteCell.x / zoomX + localCell.x;
        remoteCell.y = remoteCell.y / zoomY + localCell.y;
        remoteCell.w = remoteCell.w / zoomX;
        remoteCell.h = remoteCell.h / zoomY;
    }
}
#endif

void
Conference::mergeConfInfo(ConfInfo& newInfo, const std::string& peerURI)
{
    if (newInfo.empty()) {
        JAMI_DBG("confInfo empty, remove remoteHost");
        std::lock_guard<std::mutex> lk(confInfoMutex_);
        remoteHosts_.erase(peerURI);
        sendConferenceInfos();
        return;
    }

#ifdef ENABLE_VIDEO
    resizeRemoteParticipants(newInfo, peerURI);
#endif

    bool updateNeeded = false;
    auto it = remoteHosts_.find(peerURI);
    if (it != remoteHosts_.end()) {
        // Compare confInfo before update
        if (it->second != newInfo) {
            it->second = newInfo;
            updateNeeded = true;
        } else
            JAMI_WARN("No change in confInfo, don't update");
    } else {
        remoteHosts_.emplace(peerURI, newInfo);
        updateNeeded = true;
    }
    // Send confInfo only if needed to avoid loops
#ifdef ENABLE_VIDEO
    if (updateNeeded and videoMixer_) {
        // Trigger the layout update in the mixer because the frame resolution may
        // change from participant to conference and cause a mismatch between
        // confInfo layout and rendering layout.
        videoMixer_->updateLayout();
    }
#endif
}

std::string_view
Conference::findHostforRemoteParticipant(std::string_view uri)
{
    for (const auto& host : remoteHosts_) {
        for (const auto& p : host.second) {
            if (uri == string_remove_suffix(p.uri, '@'))
                return host.first;
        }
    }
    return "";
}

std::shared_ptr<Call>
Conference::getCallFromPeerID(std::string_view peerID)
{
    for (const auto& p : getParticipantList()) {
        auto call = getCall(p);
        if (call && string_remove_suffix(call->getPeerNumber(), '@') == peerID) {
            return call;
        }
    }
    return nullptr;
}

} // namespace jami
