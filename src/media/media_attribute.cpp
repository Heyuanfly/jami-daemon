/*
 *  Copyright (C) 2021 Savoir-faire Linux Inc.
 *
 *  Author: Mohamed Chibani <mohamed.chibani@savoirfairelinux.com>
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

#include "media/media_attribute.h"

namespace jami {

MediaAttribute::MediaAttribute(const DRing::MediaMap& mediaMap)
{
    std::pair<bool, MediaType> pairType = getMediaType(mediaMap);
    if (pairType.first)
        type_ = pairType.second;

    std::pair<bool, bool> pairBool;

    pairBool = getBoolValue(mediaMap, MediaAttributeKey::MUTED);
    if (pairBool.first)
        muted_ = pairBool.second;

    pairBool = getBoolValue(mediaMap, MediaAttributeKey::ENABLED);
    if (pairBool.first)
        enabled_ = pairBool.second;

    std::pair<bool, std::string> pairString;
    pairString = getStringValue(mediaMap, MediaAttributeKey::SOURCE);
    if (pairBool.first)
        sourceUri_ = pairString.second;

    pairString = getStringValue(mediaMap, MediaAttributeKey::LABEL);
    if (pairBool.first)
        label_ = pairString.second;
}

std::vector<MediaAttribute>
MediaAttribute::parseMediaList(const std::vector<DRing::MediaMap>& mediaList)
{
    std::vector<MediaAttribute> mediaAttrList;
    mediaAttrList.reserve(mediaList.size());

    for (auto const& mediaMap : mediaList) {
        mediaAttrList.emplace_back(MediaAttribute(mediaMap));
    }

    return mediaAttrList;
}

MediaType
MediaAttribute::stringToMediaType(const std::string& mediaType)
{
    if (mediaType.compare(MediaAttributeValue::AUDIO) == 0)
        return MediaType::MEDIA_AUDIO;
    if (mediaType.compare(MediaAttributeValue::VIDEO) == 0)
        return MediaType::MEDIA_VIDEO;
    return MediaType::MEDIA_NONE;
}

std::pair<bool, MediaType>
MediaAttribute::getMediaType(const DRing::MediaMap& map)
{
    const auto& iter = map.find(MediaAttributeKey::MEDIA_TYPE);
    if (iter == map.end()) {
        JAMI_WARN("[MEDIA_TYPE] key not found in media map");
        return {false, MediaType::MEDIA_NONE};
    }

    auto type = stringToMediaType(iter->second);
    if (type == MediaType::MEDIA_NONE) {
        JAMI_ERR("Invalid value [%s] for a media type key in media map", iter->second.c_str());
        return {false, type};
    }

    return {true, type};
}

std::pair<bool, bool>
MediaAttribute::getBoolValue(const DRing::MediaMap& map, const std::string& key)
{
    const auto& iter = map.find(key);
    if (iter == map.end()) {
        JAMI_WARN("[%s] key not found for media", key.c_str());
        return {false, false};
    }

    auto const& value = iter->second;
    if (value.compare(TRUE_STR) == 0)
        return {true, true};
    if (value.compare(FALSE_STR) == 0)
        return {true, false};

    JAMI_ERR("Invalid value %s for a boolean key", value.c_str());
    return {false, false};
}

std::pair<bool, std::string>
MediaAttribute::getStringValue(const DRing::MediaMap& map, const std::string& key)
{
    const auto& iter = map.find(key);
    if (iter == map.end()) {
        JAMI_WARN("[%s] key not found in media map", key.c_str());
        return {false, {}};
    }

    return {true, iter->second};
}

char const*
MediaAttribute::boolToString(bool val)
{
    return val ? TRUE_STR : FALSE_STR;
}

char const*
MediaAttribute::mediaTypeToString(MediaType type)
{
    if (type == MediaType::MEDIA_AUDIO)
        return MediaAttributeValue::AUDIO;
    if (type == MediaType::MEDIA_VIDEO)
        return MediaAttributeValue::VIDEO;
    return nullptr;
}

bool
MediaAttribute::hasMediaType(const std::vector<MediaAttribute>& mediaList, MediaType type)
{
    return mediaList.end()
           != std::find_if(mediaList.begin(), mediaList.end(), [type](const MediaAttribute& media) {
                  return media.type_ == type;
              });
}

DRing::MediaMap
MediaAttribute::toMediaMap(const MediaAttribute& mediaAttr)
{
    DRing::MediaMap mediaMap;

    mediaMap.emplace(MediaAttributeKey::MEDIA_TYPE, mediaTypeToString(mediaAttr.type_));
    mediaMap.emplace(MediaAttributeKey::LABEL, mediaAttr.label_);
    mediaMap.emplace(MediaAttributeKey::ENABLED, boolToString(mediaAttr.enabled_));
    mediaMap.emplace(MediaAttributeKey::MUTED, boolToString(mediaAttr.muted_));
    mediaMap.emplace(MediaAttributeKey::SOURCE, mediaAttr.sourceUri_);

    return mediaMap;
}

std::vector<DRing::MediaMap>
MediaAttribute::mediaAttributesToMediaMaps(std::vector<MediaAttribute> mediaAttrList)
{
    std::vector<DRing::MediaMap> mediaList;
    mediaAttrList.reserve(mediaAttrList.size());
    for (auto const& media : mediaAttrList) {
        mediaList.emplace_back(toMediaMap(media));
    }

    return mediaList;
}

std::string
MediaAttribute::toString(bool full) const
{
    std::ostringstream descr;
    descr << "[" << this << "] ";
    descr << "type " << (type_ == MediaType::MEDIA_AUDIO ? "[AUDIO]" : "[VIDEO]");
    descr << " ";
    descr << "muted " << (muted_ ? "[YES]" : "[NO]");
    descr << " ";
    descr << "label [" << label_ << "]";

    if (full) {
        descr << " ";
        descr << "source [" << sourceUri_ << "]";
        descr << " ";
        descr << "secure " << (secure_ ? "[YES]" : "[NO]");
    }

    return descr.str();
}
} // namespace jami