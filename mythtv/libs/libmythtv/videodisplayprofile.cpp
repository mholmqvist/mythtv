// -*- Mode: c++ -*-
#include <algorithm>
using namespace std;

#include "videodisplayprofile.h"
#include "mythcontext.h"
#include "mythdb.h"
#include "mythverbose.h"

bool ProfileItem::IsMatch(const QSize &size, float rate) const
{
    (void) rate; // we don't use the video output rate yet

    bool    match = true;
    QString cmp   = QString::null;

    for (uint i = 0; (i < 1000) && match; i++)
    {
        cmp = Get(QString("pref_cmp%1").arg(i));
        if (cmp.isEmpty())
            break;

        QStringList clist = cmp.split(" ", QString::SkipEmptyParts);
        if (clist.size() != 3)
            break;

        int width  = clist[1].toInt();
        int height = clist[2].toInt();
        cmp = clist[0];

        if (cmp == "==")
            match &= (size.width() == width) && (size.height() == height);
        else if (cmp == "!=")
            match &= (size.width() != width) && (size.height() != height);
        else if (cmp == "<=")
            match &= (size.width() <= width) && (size.height() <= height);
        else if (cmp == "<")
            match &= (size.width() <  width) && (size.height() <  height);
        else if (cmp == ">=")
            match &= (size.width() >= width) && (size.height() >= height);
        else if (cmp == ">")
            match &= (size.width() >  width) || (size.height() >  height);
        else
            match = false;
    }

    return match;
}

static QString toCommaList(const QStringList &list)
{
    QString ret = "";
    for (QStringList::const_iterator it = list.begin(); it != list.end(); ++it)
        ret += *it + ",";

    if (ret.length())
        return ret.left(ret.length()-1);

    return "";
}

bool ProfileItem::IsValid(QString *reason) const
{
    QString     decoder   = Get("pref_decoder");
    QString     renderer  = Get("pref_videorenderer");
    if (decoder.isEmpty() || renderer.isEmpty())
    {
        if (reason)
            *reason = "Need a decoder and renderer";

        return false;
    }

    QStringList decoders  = VideoDisplayProfile::GetDecoders();
    if (!decoders.contains(decoder))
    {
        if (reason)
        {
            *reason = QString("decoder %1 is not supported (supported: %2)")
                .arg(decoder).arg(toCommaList(decoders));
        }

        return false;
    }

    QStringList renderers = VideoDisplayProfile::GetVideoRenderers(decoder);
    if (!renderers.contains(renderer))
    {
        if (reason)
        {
            *reason = QString("renderer %1 is not supported "
                       "w/decoder %2 (supported: %3)")
                .arg(renderer).arg(decoder).arg(toCommaList(renderers));
        }

        return false;
    }

    QStringList deints    = VideoDisplayProfile::GetDeinterlacers(renderer);
    QString     deint0    = Get("pref_deint0");
    QString     deint1    = Get("pref_deint1");
    if (!deint0.isEmpty() && !deints.contains(deint0))
    {
        if (reason)
        {
            *reason = QString("deinterlacer %1 is not supported "
                              "w/renderer %2 (supported: %3)")
                .arg(deint0).arg(renderer).arg(toCommaList(deints));
        }

        return false;
    }

    if (!deint1.isEmpty() &&
        (!deints.contains(deint1) ||
         deint1.contains("bobdeint") ||
         deint1.contains("doublerate") ||
         deint1.contains("doubleprocess")))
    {
        if (reason)
        {
            if (deint1.contains("bobdeint") ||
                deint1.contains("doublerate") ||
                deint1.contains("doubleprocess"))
                deints.removeAll(deint1);

            *reason = QString("deinterlacer %1 is not supported w/renderer %2 "
                              "as second deinterlacer (supported: %3)")
                .arg(deint1).arg(renderer).arg(toCommaList(deints));
        }

        return false;
    }

    QStringList osds      = VideoDisplayProfile::GetOSDs(renderer);
    QString     osd       = Get("pref_osdrenderer");
    if (!osds.contains(osd))
    {
        if (reason)
        {
            *reason = QString("OSD Renderer %1 is not supported "
                              "w/renderer %2 (supported: %3)")
                .arg(osd).arg(renderer).arg(toCommaList(osds));
        }

        return false;
    }

    QString     filter    = Get("pref_filters");
    if (!filter.isEmpty() && !VideoDisplayProfile::IsFilterAllowed(renderer))
    {
        if (reason)
        {
            *reason = QString("Filter %1 is not supported w/renderer %2")
                .arg(filter).arg(renderer);
        }

        return false;
    }

    if (reason)
        *reason = QString::null;

    return true;
}

bool ProfileItem::operator< (const ProfileItem &other) const
{
    return GetPriority() < other.GetPriority();
}

QString ProfileItem::toString(void) const
{
    QString cmp0      = Get("pref_cmp0");
    QString cmp1      = Get("pref_cmp1");
    QString decoder   = Get("pref_decoder");
    uint    max_cpus  = Get("pref_max_cpus").toUInt();
    QString renderer  = Get("pref_videorenderer");
    QString osd       = Get("pref_osdrenderer");
    QString deint0    = Get("pref_deint0");
    QString deint1    = Get("pref_deint1");
    QString filter    = Get("pref_filters");
    bool    osdfade   = Get("pref_osdfade").toInt();

    QString str =  QString("cmp(%1%2) dec(%3) cpus(%4) rend(%5) ")
        .arg(cmp0).arg(QString(cmp1.isEmpty() ? "" : ",") + cmp1)
        .arg(decoder).arg(max_cpus).arg(renderer);
    str += QString("osd(%1) osdfade(%2) deint(%3,%4) filt(%5)")
        .arg(osd).arg((osdfade) ? "enabled" : "disabled")
        .arg(deint0).arg(deint1).arg(filter);

    return str;
}

//////////////////////////////////////////////////////////////////////////////

#define LOC     QString("VDP: ")
#define LOC_ERR QString("VDP, Error: ")

QMutex      VideoDisplayProfile::safe_lock(QMutex::Recursive);
bool        VideoDisplayProfile::safe_initialized = false;
safe_map_t  VideoDisplayProfile::safe_renderer;
safe_map_t  VideoDisplayProfile::safe_renderer_group;
safe_map_t  VideoDisplayProfile::safe_deint;
safe_map_t  VideoDisplayProfile::safe_osd;
safe_map_t  VideoDisplayProfile::safe_equiv_dec;
safe_list_t VideoDisplayProfile::safe_custom;
priority_map_t VideoDisplayProfile::safe_renderer_priority;
pref_map_t  VideoDisplayProfile::dec_name;

VideoDisplayProfile::VideoDisplayProfile()
    : lock(QMutex::Recursive), last_size(0,0), last_rate(0.0f),
      last_video_renderer(QString::null)
{
    QMutexLocker locker(&safe_lock);
    init_statics();

    QString hostname    = gContext->GetHostName();
    QString cur_profile = GetDefaultProfileName(hostname);
    uint    groupid     = GetProfileGroupID(cur_profile, hostname);

    item_list_t items = LoadDB(groupid);
    item_list_t::const_iterator it;
    for (it = items.begin(); it != items.end(); it++)
    {
        QString err;
        if (!(*it).IsValid(&err))
        {
            VERBOSE(VB_PLAYBACK, LOC + "Rejecting: " + (*it).toString() +
                    "\n\t\t\t" + err);

            continue;
        }
        VERBOSE(VB_PLAYBACK, LOC + "Accepting: " + (*it).toString());
        all_pref.push_back(*it);
    }

    SetInput(QSize(2048, 2048));
    SetOutput(60.0f);
}

VideoDisplayProfile::~VideoDisplayProfile()
{
}

void VideoDisplayProfile::SetInput(const QSize &size)
{
    QMutexLocker locker(&lock);
    if (size != last_size)
    {
        last_size = size;
        LoadBestPreferences(last_size, last_rate);
    }
}

void VideoDisplayProfile::SetOutput(float framerate)
{
    QMutexLocker locker(&lock);
    if (framerate != last_rate)
    {
        last_rate = framerate;
        LoadBestPreferences(last_size, last_rate);
    }
}

void VideoDisplayProfile::SetVideoRenderer(const QString &video_renderer)
{
    QMutexLocker locker(&lock);

    VERBOSE(VB_PLAYBACK, LOC +
            QString("SetVideoRenderer(%1)").arg(video_renderer));

    last_video_renderer = video_renderer;
    last_video_renderer.detach();

    if (video_renderer == GetVideoRenderer())
    {
        VERBOSE(VB_PLAYBACK, LOC +
                QString("SetVideoRender(%1) == GetVideoRenderer()")
                .arg(video_renderer));
        return; // already made preferences safe...
    }

    // Make preferences safe...

    VERBOSE(VB_PLAYBACK, LOC + "Old preferences: " + toString());

    SetPreference("pref_videorenderer", video_renderer);

    QStringList osds = GetOSDs(video_renderer);
    if (!osds.contains(GetOSDRenderer()))
        SetPreference("pref_osdrenderer", osds[0]);

    QStringList deints = GetDeinterlacers(video_renderer);
    if (!deints.contains(GetDeinterlacer()))
        SetPreference("pref_deint0", deints[0]);
    if (!deints.contains(GetFallbackDeinterlacer()))
        SetPreference("pref_deint1", deints[0]);
    if (GetFallbackDeinterlacer().contains("bobdeint") ||
        GetFallbackDeinterlacer().contains("doublerate") ||
        GetFallbackDeinterlacer().contains("doubleprocess"))
    {
        SetPreference("pref_deint1", deints[1]);
    }

    SetPreference("pref_filters", "");

    VERBOSE(VB_PLAYBACK, LOC + "New preferences: " + toString());
}

bool VideoDisplayProfile::CheckVideoRendererGroup(const QString renderer)
{
    if (last_video_renderer == renderer ||
        last_video_renderer == "null")
        return true;

    VERBOSE(VB_PLAYBACK, LOC +
        QString("Preferred video renderer: %1 (current: %2)")
                .arg(renderer).arg(last_video_renderer));

    safe_map_t::const_iterator it = safe_renderer_group.begin();
    for (; it != safe_renderer_group.end(); it++)
        if (it->contains(last_video_renderer) &&
            it->contains(renderer))
            return true;
    return false;
}
    
bool VideoDisplayProfile::IsDecoderCompatible(const QString &decoder)
{
    const QString dec = GetDecoder();
    if (dec == decoder)
        return true;

    QMutexLocker locker(&safe_lock);
    return (safe_equiv_dec[dec].contains(decoder));
}

QString VideoDisplayProfile::GetFilteredDeint(const QString &override)
{
    QString renderer = GetActualVideoRenderer();
    QString deint    = GetDeinterlacer();

    QMutexLocker locker(&lock);

    if (!override.isEmpty() && GetDeinterlacers(renderer).contains(override))
        deint = override;

    VERBOSE(VB_PLAYBACK, LOC + QString("GetFilteredDeint(%1) : %2 -> '%3'")
            .arg(override).arg(renderer).arg(deint));

    deint.detach();
    return deint;
}

QString VideoDisplayProfile::GetPreference(const QString &key) const
{
    QMutexLocker locker(&lock);

    if (key.isEmpty())
        return QString::null;

    pref_map_t::const_iterator it = pref.find(key);
    if (it == pref.end())
        return QString::null;

    QString pref = *it;
    pref.detach();
    return pref;
}

void VideoDisplayProfile::SetPreference(
    const QString &key, const QString &value)
{
    QMutexLocker locker(&lock);

    if (!key.isEmpty())
    {
        QString tmp = value;
        tmp.detach();
        pref[key] = tmp;
    }
}

item_list_t::const_iterator VideoDisplayProfile::FindMatch(
    const QSize &size, float rate)
{
    item_list_t::const_iterator it = all_pref.begin();
    for (; it != all_pref.end(); ++it)
    {
        if ((*it).IsMatch(size, rate))
            return it;
    }

    return all_pref.end();
}

void VideoDisplayProfile::LoadBestPreferences(const QSize &size,
                                              float framerate)
{
    VERBOSE(VB_PLAYBACK, LOC + QString("LoadBestPreferences(%1x%2, %3)")
            .arg(size.width()).arg(size.height()).arg(framerate));

    pref.clear();
    item_list_t::const_iterator it = FindMatch(size, framerate);
    if (it != all_pref.end())
        pref = (*it).GetAll();
}

////////////////////////////////////////////////////////////////////////////
// static methods

item_list_t VideoDisplayProfile::LoadDB(uint groupid)
{
    ProfileItem tmp;
    item_list_t list;

    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "SELECT profileid, value, data "
        "FROM displayprofiles "
        "WHERE profilegroupid = :GROUPID "
        "ORDER BY profileid");
    query.bindValue(":GROUPID", groupid);
    if (!query.exec())
    {
        MythDB::DBError("loaddb 1", query);
        return list;
    }

    uint profileid = 0;
    while (query.next())
    {
        if (query.value(0).toUInt() != profileid)
        {
            if (profileid)
            {
                tmp.SetProfileID(profileid);
                list.push_back(tmp);
            }
            tmp.Clear();
            profileid = query.value(0).toUInt();
        }
        tmp.Set(query.value(1).toString(), query.value(2).toString());
    }
    if (profileid)
    {
        tmp.SetProfileID(profileid);
        list.push_back(tmp);
    }

    sort(list.begin(), list.end());
    return list;
}

bool VideoDisplayProfile::DeleteDB(uint groupid, const item_list_t &items)
{
    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "DELETE FROM displayprofiles "
        "WHERE profilegroupid = :GROUPID   AND "
        "      profileid      = :PROFILEID");

    bool ok = true;
    item_list_t::const_iterator it = items.begin();
    for (; it != items.end(); ++it)
    {
        if (!(*it).GetProfileID())
            continue;

        query.bindValue(":GROUPID",   groupid);
        query.bindValue(":PROFILEID", (*it).GetProfileID());
        if (!query.exec())
        {
            MythDB::DBError("vdp::deletedb", query);
            ok = false;
        }
    }

    return ok;
}

bool VideoDisplayProfile::SaveDB(uint groupid, item_list_t &items)
{
    MSqlQuery query(MSqlQuery::InitCon());

    MSqlQuery update(MSqlQuery::InitCon());
    update.prepare(
        "UPDATE displayprofiles "
        "SET data = :DATA "
        "WHERE profilegroupid = :GROUPID   AND "
        "      profileid      = :PROFILEID AND "
        "      value          = :VALUE");

    MSqlQuery insert(MSqlQuery::InitCon());
    insert.prepare(
        "INSERT INTO displayprofiles "
        " ( profilegroupid,  profileid,  value,  data) "
        "VALUES "
        " (:GROUPID,        :PROFILEID, :VALUE, :DATA) ");


    bool ok = true;
    item_list_t::iterator it = items.begin();
    for (; it != items.end(); ++it)
    {
        pref_map_t list = (*it).GetAll();
        if (list.begin() == list.end())
            continue;

        pref_map_t::const_iterator lit = list.begin();

        if (!(*it).GetProfileID())
        {
            // create new profileid
            if (!query.exec("SELECT MAX(profileid) FROM displayprofiles"))
            {
                MythDB::DBError("save_profile 1", query);
                ok = false;
                continue;
            }
            else if (query.next())
            {
                (*it).SetProfileID(query.value(0).toUInt() + 1);
            }

            for (; lit != list.end(); ++lit)
            {
                if ((*lit).isEmpty())
                    continue;

                insert.bindValue(":GROUPID",   groupid);
                insert.bindValue(":PROFILEID", (*it).GetProfileID());
                insert.bindValue(":VALUE",     lit.key());
                insert.bindValue(":DATA",      (*lit));
                if (!insert.exec())
                {
                    MythDB::DBError("save_profile 2", insert);
                    ok = false;
                    continue;
                }
            }
            continue;
        }

        for (; lit != list.end(); ++lit)
        {
            query.prepare(
                "SELECT count(*) "
                "FROM displayprofiles "
                "WHERE  profilegroupid = :GROUPID AND "
                "       profileid      = :PROFILEID AND "
                "       value          = :VALUE");
            query.bindValue(":GROUPID",   groupid);
            query.bindValue(":PROFILEID", (*it).GetProfileID());
            query.bindValue(":VALUE",     lit.key());

            if (!query.exec())
            {
                MythDB::DBError("save_profile 3", query);
                ok = false;
                continue;
            }
            else if (query.next() && (1 == query.value(0).toUInt()))
            {
                update.bindValue(":GROUPID",   groupid);
                update.bindValue(":PROFILEID", (*it).GetProfileID());
                update.bindValue(":VALUE",     lit.key());
                update.bindValue(":DATA",      (*lit));
                if (!update.exec())
                {
                    MythDB::DBError("save_profile 5", update);
                    ok = false;
                    continue;
                }
            }
            else
            {
                insert.bindValue(":GROUPID",   groupid);
                insert.bindValue(":PROFILEID", (*it).GetProfileID());
                insert.bindValue(":VALUE",     lit.key());
                insert.bindValue(":DATA",      (*lit));
                if (!insert.exec())
                {
                    MythDB::DBError("save_profile 4", insert);
                    ok = false;
                    continue;
                }
            }
        }
    }

    return ok;
}

QStringList VideoDisplayProfile::GetDecoders(void)
{
    QStringList list;

    list += "ffmpeg";
    list += "libmpeg2";
    list += "xvmc";
    list += "xvmc-vld";
    list += "macaccel";
    list += "ivtv";
    list += "vdpau";

    return list;
}

QStringList VideoDisplayProfile::GetDecoderNames(void)
{
    QStringList list;

    const QStringList decs = GetDecoders();
    QStringList::const_iterator it = decs.begin();
    for (; it != decs.end(); ++it)
        list += GetDecoderName(*it);

    return list;
}

QString VideoDisplayProfile::GetDecoderName(const QString &decoder)
{
    if (decoder.isEmpty())
        return "";

    QMutexLocker locker(&safe_lock);
    if (dec_name.empty())
    {
        dec_name["ffmpeg"]   = QObject::tr("Standard");
        dec_name["libmpeg2"] = QObject::tr("libmpeg2");
        dec_name["xvmc"]     = QObject::tr("Standard XvMC");
        dec_name["xvmc-vld"] = QObject::tr("VIA XvMC");
        dec_name["macaccel"] = QObject::tr("Mac hardware acceleration");
        dec_name["ivtv"]     = QObject::tr("PVR-350 decoder");
        dec_name["vdpau"]    = QObject::tr("NVidia VDPAU acceleration");
    }

    QString ret = decoder;
    pref_map_t::const_iterator it = dec_name.find(decoder);
    if (it != dec_name.end())
        ret = *it;

    ret.detach();
    return ret;
}


QString VideoDisplayProfile::GetDecoderHelp(QString decoder)
{
    QString msg = QObject::tr("Decoder to use to play back MPEG2 video.");

    if (decoder.isEmpty())
        return msg;

    msg += "\n";

    if (decoder == "ffmpeg")
        msg += QObject::tr("Standard will use ffmpeg library.");

    if (decoder == "libmpeg2")
        msg +=  QObject::tr(
            "libmpeg2 is slower on almost all processors than ffmpeg "
            "and breaks caption decoding. Use at your own risk!");

    if (decoder == "xvmc")
        msg += QObject::tr(
            "Standard XvMC will use XvMC API 1.0 to "
            "play back video; this is fast, but does not "
            "work well with HDTV sized frames.");

    if (decoder == "xvmc-vld")
        msg += QObject::tr("VIA XvMC will use the VIA VLD XvMC extension.");


    if (decoder == "macaccel")
        msg += QObject::tr(
            "Mac hardware will try to use the graphics "
            "processor - this may hang or crash your Mac!");

    if (decoder == "ivtv")
        msg += QObject::tr(
            "MythTV can use the PVR-350's TV out and MPEG decoder for "
            "high quality playback.  This requires that the ivtv-fb "
            "kernel module is also loaded and configured properly.");

    if (decoder == "vdpau")
        msg += QObject::tr(
            "VDPAU will attempt to use the graphics hardware to "
            "accelerate video decoding and playback.");

    return msg;
}

QString VideoDisplayProfile::GetDeinterlacerName(const QString short_name)
{
    if ("none" == short_name)
        return QObject::tr("None");
    else if ("linearblend" == short_name)
        return QObject::tr("Linear blend");
    else if ("kerneldeint" == short_name)
        return QObject::tr("Kernel");
    else if ("kerneldoubleprocessdeint" == short_name)
        return QObject::tr("Kernel (2x)");
    else if ("greedyhdeint" == short_name)
        return QObject::tr("Greedy HighMotion");
    else if ("greedyhdoubleprocessdeint" == short_name)
        return QObject::tr("Greedy HighMotion (2x)");
    else if ("yadifdeint" == short_name)
        return QObject::tr("Yadif");
    else if ("yadifdoubleprocessdeint" == short_name)
        return QObject::tr("Yadif (2x)");
    else if ("bobdeint" == short_name)
        return QObject::tr("Bob (2x)");
    else if ("onefield" == short_name)
        return QObject::tr("One field");
    else if ("fieldorderdoubleprocessdeint" == short_name)
        return QObject::tr("Interlaced (2x)");
    else if ("opengllinearblend" == short_name)
        return QObject::tr("Linear blend (HW)");
    else if ("openglkerneldeint" == short_name)
        return QObject::tr("Kernel (HW)");
    else if ("openglbobdeint" == short_name)
        return QObject::tr("Bob (2x, HW)");
    else if ("openglonefield" == short_name)
        return QObject::tr("One field (HW)");
    else if ("opengldoubleratekerneldeint" == short_name)
        return QObject::tr("Kernel (2x, HW)");
    else if ("opengldoubleratelinearblend" == short_name)
        return QObject::tr("Linear blend (2x, HW)");
    else if ("opengldoubleratefieldorder" == short_name)
        return QObject::tr("Interlaced (2x, Hw)");
    else if ("opengldoublerateyadif" == short_name)
        return QObject::tr("Yadif (2x, Hw)");
    else if ("openglyadif" == short_name)
        return QObject::tr("Yadif (Hw)");
    else if ("vdpauonefield" == short_name)
        return QObject::tr("One Field (1x, Hw)");
    else if ("vdpaubobdeint" == short_name)
        return QObject::tr("Bob (2x, Hw)");
    else if ("vdpaubasic" == short_name)
        return QObject::tr("Temporal (1x, Hw)");
    else if ("vdpaubasicdoublerate" == short_name)
        return QObject::tr("Temporal (2x, Hw)");
    else if ("vdpauadvanced" == short_name)
        return QObject::tr("Advanced (1x, Hw)");
    else if ("vdpauadvanceddoublerate" == short_name)
        return QObject::tr("Advanced (2x, Hw)");

    return "";
}

QStringList VideoDisplayProfile::GetProfiles(const QString &hostname)
{
    QStringList list;
    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "SELECT name "
        "FROM displayprofilegroups "
        "WHERE hostname = :HOST ");
    query.bindValue(":HOST", hostname);
    if (!query.exec() || !query.isActive())
        MythDB::DBError("get_profiles", query);
    else
    {
        while (query.next())
            list += query.value(0).toString();
    }
    return list;
}

QString VideoDisplayProfile::GetDefaultProfileName(const QString &hostname)
{
    QString tmp =
        gContext->GetSettingOnHost("DefaultVideoPlaybackProfile", hostname);

    QStringList profiles = GetProfiles(hostname);

    tmp = (profiles.contains(tmp)) ? tmp : QString::null;

    if (tmp.isEmpty())
    {
        if (profiles.size())
            tmp = profiles[0];

        tmp = (profiles.contains("CPU+")) ? "CPU+" : tmp;

        if (!tmp.isEmpty())
        {
            gContext->SaveSettingOnHost(
                "DefaultVideoPlaybackProfile", tmp, hostname);
        }
    }

    return tmp;
}

void VideoDisplayProfile::SetDefaultProfileName(
    const QString &profilename, const QString &hostname)
{
    gContext->SaveSettingOnHost(
        "DefaultVideoPlaybackProfile", profilename, hostname);
}

uint VideoDisplayProfile::GetProfileGroupID(const QString &profilename,
                                            const QString &hostname)
{
    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "SELECT profilegroupid "
        "FROM displayprofilegroups "
        "WHERE name     = :NAME AND "
        "      hostname = :HOST ");
    query.bindValue(":NAME", profilename);
    query.bindValue(":HOST", hostname);

    if (!query.exec() || !query.isActive())
        MythDB::DBError("get_profile_group_id", query);
    else if (query.next())
        return query.value(0).toUInt();

    return 0;
}

void VideoDisplayProfile::DeleteProfiles(const QString &hostname)
{
    MSqlQuery query(MSqlQuery::InitCon());
    MSqlQuery query2(MSqlQuery::InitCon());
    query.prepare(
        "SELECT profilegroupid "
        "FROM displayprofilegroups "
        "WHERE hostname = :HOST ");
    query.bindValue(":HOST", hostname);
    if (!query.exec() || !query.isActive())
        MythDB::DBError("delete_profiles 1", query);
    else
    {
        while (query.next())
        {
            query2.prepare("DELETE FROM displayprofiles "
                           "WHERE profilegroupid = :PROFID");
            query2.bindValue(":PROFID", query.value(0).toUInt());
            if (!query2.exec())
                MythDB::DBError("delete_profiles 2", query2);
        }
    }
    query.prepare("DELETE FROM displayprofilegroups WHERE hostname = :HOST");
    query.bindValue(":HOST", hostname);
    if (!query.exec() || !query.isActive())
        MythDB::DBError("delete_profiles 3", query);
}

//displayprofilegroups pk(name, hostname), uk(profilegroupid)
//displayprofiles      k(profilegroupid), k(profileid), value, data

void VideoDisplayProfile::CreateProfile(
    uint groupid, uint priority,
    QString cmp0, uint width0, uint height0,
    QString cmp1, uint width1, uint height1,
    QString decoder, uint max_cpus, QString videorenderer,
    QString osdrenderer, bool osdfade,
    QString deint0, QString deint1, QString filters)
{
    MSqlQuery query(MSqlQuery::InitCon());

    if (cmp0.isEmpty() && cmp1.isEmpty())
        return;

    // create new profileid
    uint profileid = 1;
    if (!query.exec("SELECT MAX(profileid) FROM displayprofiles"))
        MythDB::DBError("create_profile 1", query);
    else if (query.next())
        profileid = query.value(0).toUInt() + 1;

    query.prepare(
        "INSERT INTO displayprofiles "
        "VALUES (:GRPID, :PROFID, 'pref_priority', :PRIORITY)");
    query.bindValue(":GRPID",    groupid);
    query.bindValue(":PROFID",   profileid);
    query.bindValue(":PRIORITY", priority);
    if (!query.exec())
        MythDB::DBError("create_profile 2", query);

    QStringList queryValue;
    QStringList queryData;

    if (!cmp0.isEmpty())
    {
        queryValue += "pref_cmp0";
        queryData  += QString("%1 %2 %3").arg(cmp0).arg(width0).arg(height0);
    }

    if (!cmp1.isEmpty())
    {
        queryValue += QString("pref_cmp%1").arg(cmp0.isEmpty() ? 0 : 1);
        queryData  += QString("%1 %2 %3").arg(cmp1).arg(width1).arg(height1);
    }

    queryValue += "pref_decoder";
    queryData  += decoder;

    queryValue += "pref_max_cpus";
    queryData  += QString::number(max_cpus);

    queryValue += "pref_videorenderer";
    queryData  += videorenderer;

    queryValue += "pref_osdrenderer";
    queryData  += osdrenderer;

    queryValue += "pref_osdfade";
    queryData  += (osdfade) ? "1" : "0";

    queryValue += "pref_deint0";
    queryData  += deint0;

    queryValue += "pref_deint1";
    queryData  += deint1;

    queryValue += "pref_filters";
    queryData  += filters;

    QStringList::const_iterator itV = queryValue.begin();
    QStringList::const_iterator itD = queryData.begin();
    for (; itV != queryValue.end() && itD != queryData.end(); ++itV,++itD)
    {
        query.prepare(
            "INSERT INTO displayprofiles "
            "VALUES (:GRPID, :PROFID, :VALUE, :DATA)");
        query.bindValue(":GRPID",  groupid);
        query.bindValue(":PROFID", profileid);
        query.bindValue(":VALUE",  *itV);
        query.bindValue(":DATA",   *itD);
        if (!query.exec())
            MythDB::DBError("create_profile 3", query);
    }
}

uint VideoDisplayProfile::CreateProfileGroup(
    const QString &profilename, const QString &hostname)
{
    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "INSERT INTO displayprofilegroups (name, hostname) "
        "VALUES (:NAME,:HOST)");

    query.bindValue(":NAME", profilename);
    query.bindValue(":HOST", hostname);

    if (!query.exec())
    {
        MythDB::DBError("create_profile_group", query);
        return 0;
    }

    return GetProfileGroupID(profilename, hostname);
}

bool VideoDisplayProfile::DeleteProfileGroup(
    const QString &groupname, const QString &hostname)
{
    bool ok = true;
    MSqlQuery query(MSqlQuery::InitCon());
    MSqlQuery query2(MSqlQuery::InitCon());

    query.prepare(
        "SELECT profilegroupid "
        "FROM displayprofilegroups "
        "WHERE name     = :NAME AND "
        "      hostname = :HOST ");

    query.bindValue(":NAME", groupname);
    query.bindValue(":HOST", hostname);

    if (!query.exec() || !query.isActive())
    {
        MythDB::DBError("delete_profile_group 1", query);
        ok = false;
    }
    else
    {
        while (query.next())
        {
            query2.prepare("DELETE FROM displayprofiles "
                           "WHERE profilegroupid = :PROFID");
            query2.bindValue(":PROFID", query.value(0).toUInt());
            if (!query2.exec())
            {
                MythDB::DBError("delete_profile_group 2", query2);
                ok = false;
            }
        }
    }

    query.prepare(
        "DELETE FROM displayprofilegroups "
        "WHERE name     = :NAME AND "
        "      hostname = :HOST");

    query.bindValue(":NAME", groupname);
    query.bindValue(":HOST", hostname);

    if (!query.exec())
    {
        MythDB::DBError("delete_profile_group 3", query);
        ok = false;
    }

    return ok;
}

void VideoDisplayProfile::CreateOldProfiles(const QString &hostname)
{
    (void) QObject::tr("CPU++", "Sample: No hardware assist");
    DeleteProfileGroup("CPU++", hostname);
    uint groupid = CreateProfileGroup("CPU++", hostname);
    CreateProfile(groupid, 1, ">", 0, 0, "", 0, 0,
                  "ffmpeg", 1, "xv-blit", "softblend", true,
                  "bobdeint", "linearblend", "");
    CreateProfile(groupid, 2, ">", 0, 0, "", 0, 0,
                  "ffmpeg", 1, "quartz-blit", "softblend", true,
                  "linearblend", "linearblend", "");

    (void) QObject::tr("CPU+", "Sample: Hardware assist HD only");
    DeleteProfileGroup("CPU+", hostname);
    groupid = CreateProfileGroup("CPU+", hostname);
    CreateProfile(groupid, 1, "<=", 720, 576, ">", 0, 0,
                  "ffmpeg", 1, "xv-blit", "softblend", true,
                  "bobdeint", "linearblend", "");
    CreateProfile(groupid, 2, "<=", 1280, 720, ">", 720, 576,
                  "xvmc", 1, "xvmc-blit", "opengl", true,
                  "bobdeint", "onefield", "");
    CreateProfile(groupid, 3, "<=", 1280, 720, ">", 720, 576,
                  "libmpeg2", 1, "xv-blit", "softblend", true,
                  "bobdeint", "onefield", "");
    CreateProfile(groupid, 4, ">", 0, 0, "", 0, 0,
                  "xvmc", 1, "xvmc-blit", "ia44blend", false,
                  "bobdeint", "onefield", "");
    CreateProfile(groupid, 5, ">", 0, 0, "", 0, 0,
                  "libmpeg2", 1, "xv-blit", "chromakey", false,
                  "bobdeint", "onefield", "");

    (void) QObject::tr("CPU--", "Sample: Hardware assist all");
    DeleteProfileGroup("CPU--", hostname);
    groupid = CreateProfileGroup("CPU--", hostname);
    CreateProfile(groupid, 1, "<=", 720, 576, ">", 0, 0,
                  "ivtv", 1, "ivtv", "ivtv", true,
                  "none", "none", "");
    CreateProfile(groupid, 2, "<=", 720, 576, ">", 0, 0,
                  "xvmc", 1, "xvmc-blit", "ia44blend", false,
                  "bobdeint", "onefield", "");
    CreateProfile(groupid, 3, "<=", 1280, 720, ">", 720, 576,
                  "xvmc", 1, "xvmc-blit", "ia44blend", false,
                  "bobdeint", "onefield", "");
    CreateProfile(groupid, 4, ">", 0, 0, "", 0, 0,
                  "xvmc", 1, "xvmc-blit", "ia44blend", false,
                  "bobdeint", "onefield", "");
    CreateProfile(groupid, 5, ">", 0, 0, "", 0, 0,
                  "libmpeg2", 1, "xv-blit", "chromakey", false,
                  "none", "none", "");
}

void VideoDisplayProfile::CreateNewProfiles(const QString &hostname)
{
    (void) QObject::tr("High Quality", "Sample: high quality");
    DeleteProfileGroup("High Quality", hostname);
    uint groupid = CreateProfileGroup("High Quality", hostname);
    CreateProfile(groupid, 1, ">=", 1920, 1080, "", 0, 0,
                  "ffmpeg", 2, "xv-blit", "softblend", true,
                  "linearblend", "linearblend", "");
    CreateProfile(groupid, 2, ">", 0, 0, "", 0, 0,
                  "ffmpeg", 1, "xv-blit", "softblend", true,
                  "yadifdoubleprocessdeint", "yadifdeint", "");
    CreateProfile(groupid, 3, ">=", 1920, 1080, "", 0, 0,
                  "ffmpeg", 2, "quartz-blit", "softblend", true,
                  "linearblend", "linearblend", "");
    CreateProfile(groupid, 4, ">", 0, 0, "", 0, 0,
                  "ffmpeg", 1, "quartz-blit", "softblend", true,
                  "yadifdoubleprocessdeint", "yadifdeint", "");

    (void) QObject::tr("Normal", "Sample: average quality");
    DeleteProfileGroup("Normal", hostname);
    groupid = CreateProfileGroup("Normal", hostname);
    CreateProfile(groupid, 1, ">=", 1280, 720, "", 0, 0,
                  "ffmpeg", 1, "xv-blit", "softblend", false,
                  "linearblend", "linearblend", "");
    CreateProfile(groupid, 2, ">", 0, 0, "", 0, 0,
                  "ffmpeg", 1, "xv-blit", "softblend", true,
                  "greedyhdoubleprocessdeint", "kerneldeint", "");
    CreateProfile(groupid, 3, ">=", 1280, 720, "", 0, 0,
                  "ffmpeg", 1, "quartz-blit", "softblend", false,
                  "linearblend", "linearblend", "");
    CreateProfile(groupid, 4, ">", 0, 0, "", 0, 0,
                  "ffmpeg", 1, "quartz-blit", "softblend", true,
                  "greedyhdoubleprocessdeint", "kerneldeint", "");

    (void) QObject::tr("Slim", "Sample: low CPU usage");
    DeleteProfileGroup("Slim", hostname);
    groupid = CreateProfileGroup("Slim", hostname);
    CreateProfile(groupid, 1, ">=", 1280, 720, "", 0, 0,
                  "ffmpeg", 1, "xv-blit", "softblend", false,
                  "onefield", "onefield", "");
    CreateProfile(groupid, 2, ">", 0, 0, "", 0, 0,
                  "ffmpeg", 1, "xv-blit", "softblend", true,
                  "linearblend", "linearblend", "");
    CreateProfile(groupid, 3, ">=", 1280, 720, "", 0, 0,
                  "ffmpeg", 1, "quartz-blit", "softblend", false,
                  "onefield", "onefield", "");
    CreateProfile(groupid, 4, ">", 0, 0, "", 0, 0,
                  "ffmpeg", 1, "quartz-blit", "softblend", true,
                  "linearblend", "linearblend", "");
}

void VideoDisplayProfile::CreateVDPAUProfiles(const QString &hostname)
{
    (void) QObject::tr("VDPAU High Quality", "Sample: VDPAU high quality");
    DeleteProfileGroup("VDPAU High Quality", hostname);
    uint groupid = CreateProfileGroup("VDPAU High Quality", hostname);
    CreateProfile(groupid, 1, ">", 0, 0, "", 0, 0,
                  "vdpau", 1, "vdpau", "vdpau", true,
                  "vdpauadvanceddoublerate", "vdpauadvanced",
                  "vdpaucolorspace=auto");

    (void) QObject::tr("VDPAU Normal", "Sample: VDPAU average quality");
    DeleteProfileGroup("VDPAU Normal", hostname);
    groupid = CreateProfileGroup("VDPAU Normal", hostname);
    CreateProfile(groupid, 1, ">=", 0, 720, "", 0, 0,
                  "vdpau", 1, "vdpau", "vdpau", true,
                  "vdpaubasicdoublerate", "vdpaubasic",
                  "vdpaucolorspace=auto");
    CreateProfile(groupid, 2, ">", 0, 0, "", 0, 0,
                  "vdpau", 1, "vdpau", "vdpau", true,
                  "vdpauadvanceddoublerate", "vdpauadvanced",
                  "vdpaucolorspace=auto");

    (void) QObject::tr("VDPAU Slim", "Sample: VDPAU low power GPU");
    DeleteProfileGroup("VDPAU Slim", hostname);
    groupid = CreateProfileGroup("VDPAU Slim", hostname);
    CreateProfile(groupid, 1, ">", 0, 0, "", 0, 0,
                  "vdpau", 1, "vdpau", "vdpau", false,
                  "vdpaubobdeint", "vdpauonefield", "vdpauskipchroma,vdpaucolorspace=auto");
}

void VideoDisplayProfile::CreateProfiles(const QString &hostname)
{
    CreateOldProfiles(hostname);
    CreateNewProfiles(hostname);
}

QStringList VideoDisplayProfile::GetVideoRenderers(const QString &decoder)
{
    QMutexLocker locker(&safe_lock);
    init_statics();

    safe_map_t::const_iterator it = safe_renderer.find(decoder);
    QStringList tmp;
    if (it != safe_renderer.end())
        tmp = *it;

    tmp.detach();
    return tmp;
}

QString VideoDisplayProfile::GetVideoRendererHelp(const QString &renderer)
{
    QString msg = QObject::tr("Video rendering method");

    if (renderer.isEmpty())
        return msg;

    if (renderer == "null")
        msg = QObject::tr(
            "Render video offscreen. Used internally.");

    if (renderer == "xlib")
        msg = QObject::tr(
            "Use X11 pixel copy to render video. This is not recommended if "
            "any other option is available. The video will not be scaled to "
            "fit the screen. This will work with all X11 servers, local "
            "and remote.");

    if (renderer == "xshm")
        msg = QObject::tr(
            "Use X11 shared memory pixel transfer to render video. This is "
            "only recommended over the X11 pixel copy renderer. The video "
            "will not be scaled to fit the screen. This works with most "
            "local X11 servers.");

    if (renderer == "xv-blit")
        msg = QObject::tr(
            "This is the standard video renderer for X11 systems. It uses "
            "XVideo hardware assist for scaling, color conversion. If the "
            "hardware offers picture controls the renderer supports them.");

    if (renderer == "xvmc-blit")
        msg = QObject::tr(
            "This is the standard video renderer for XvMC decoders. It uses "
            "XVideo hardware assist for scaling, color conversion and "
            "when available offers XVideo picture controls.");

    if (renderer == "directfb")
        msg = QObject::tr(
            "This video renderer uses DirectFB for scaling and color "
            "conversion. It is not as feature rich as the standard video "
            "renderer, but can run on Linux hardware without an X11 server.");

    if (renderer == "direct3d")
        msg = QObject::tr(
            "Windows video renderer based on Direct3D. Requires "
            "video card compatible with Direct3D 9. This is the preferred "
            "renderer for current Windows systems.");

    if (renderer == "quartz-blit")
        msg = QObject::tr(
            "This is the standard video render for Macintosh OS X systems.");

    if (renderer == "quartz-accel")
        msg = QObject::tr(
            "This is the only video renderer for the MacAccel decoder.");

    if (renderer == "ivtv")
        msg = QObject::tr(
            "This is only video renderer for the PVR-350 decoder.");

    if (renderer == "opengl")
    {
        msg = QObject::tr(
            "This video renderer uses OpenGL for scaling and color conversion "
            "and can offer limited picture contols. This requires a faster "
            "GPU than XVideo. Also, when enabled, picture controls consume "
            "additional resources.");
    }

    if (renderer == "vdpau")
    {
        msg = QObject::tr(
            "This is the only video renderer for NVidia VDPAU decoding.");
    }

    return msg;
}

QString VideoDisplayProfile::GetPreferredVideoRenderer(const QString &decoder)
{
    return GetBestVideoRenderer(GetVideoRenderers(decoder));
}

QStringList VideoDisplayProfile::GetDeinterlacers(
    const QString &video_renderer)
{
    QMutexLocker locker(&safe_lock);
    init_statics();

    safe_map_t::const_iterator it = safe_deint.find(video_renderer);
    QStringList tmp;
    if (it != safe_deint.end())
        tmp = *it;

    tmp.detach();
    return tmp;
}

QString VideoDisplayProfile::GetDeinterlacerHelp(const QString &deint)
{
    if (deint.isEmpty())
        return "";

    QString msg = "";

    QString kDoubleRateMsg =
        QObject::tr(
            "This deinterlacer requires the display to be capable "
            "of twice the frame rate as the source video.");

    QString kNoneMsg =
        QObject::tr("Perform no deinterlacing.") + " " +
        QObject::tr(
            "Use this with an interlaced display whose "
            "resolution exactly matches the video size. "
            "This is incompatible with MythTV zoom modes.");

    QString kOneFieldMsg = QObject::tr(
        "Shows only one of the two fields in the frame. "
        "This looks good when displaying a high motion "
        "1080i video on a 720p display.");

    QString kBobMsg = QObject::tr(
        "Shows one field of the frame followed by the "
        "other field displaced vertically.") + " " +
        kDoubleRateMsg;

    QString kLinearBlendMsg = QObject::tr(
        "Blends the odd and even fields linearly into one frame.");

    QString kKernelMsg = QObject::tr(
        "This filter disables deinterlacing when the two fields are "
        "similar, and performs linear deinterlacing otherwise.");

    QString kUsingGPU = QObject::tr("(Hardware Accelerated)");

    QString kGreedyHMsg = QObject::tr(
        "This deinterlacer uses several fields to reduce motion blur. "
        "It has increased CPU requirements.");

    QString kYadifMsg = QObject::tr(
        "This deinterlacer uses several fields to reduce motion blur. "
        "It has increased CPU requirements.");

    QString kFieldOrderMsg = QObject::tr(
        "This deinterlacer attempts to syncronise with interlaced displays "
        "whose size and refresh rate exactly match the video source. "
        "It has low CPU requirements.");

    QString kBasicMsg = QObject::tr(
        "This deinterlacer uses several fields to reduce motion blur. ");

    QString kAdvMsg = QObject::tr(
        "This deinterlacer uses multiple fields to reduce motion blur "
        "and smooth edges. ");

    if (deint == "none")
        msg = kNoneMsg;
    else if (deint == "onefield")
        msg = kOneFieldMsg;
    else if (deint == "bobdeint")
        msg = kBobMsg;
    else if (deint == "linearblend")
        msg = kLinearBlendMsg;
    else if (deint == "kerneldeint")
        msg = kKernelMsg;
    else if (deint == "kerneldoubleprocessdeint")
        msg = kKernelMsg + " " + kDoubleRateMsg;
    else if (deint == "openglonefield")
        msg = kOneFieldMsg + " " + kUsingGPU;
    else if (deint == "openglbobdeint")
        msg = kBobMsg + " " + kUsingGPU;
    else if (deint == "opengllinearblend")
        msg = kLinearBlendMsg + " " + kUsingGPU;
    else if (deint == "openglkerneldeint")
        msg = kKernelMsg + " " + kUsingGPU;
    else if (deint == "openglyadif")
        msg = kYadifMsg + " " + kUsingGPU;
    else if (deint == "opengldoubleratelinearblend")
        msg = kLinearBlendMsg + " " +  kDoubleRateMsg + " " + kUsingGPU;
    else if (deint == "opengldoubleratekerneldeint")
        msg = kKernelMsg + " " +  kDoubleRateMsg + " " + kUsingGPU;
    else if (deint == "opengldoubleratefieldorder")
        msg = kFieldOrderMsg + " " +  kDoubleRateMsg  + " " + kUsingGPU;
    else if (deint == "greedyhdeint")
        msg = kGreedyHMsg;
    else if (deint == "greedyhdoubleprocessdeint")
        msg = kGreedyHMsg + " " +  kDoubleRateMsg;
    else if (deint == "yadifdeint")
        msg = kYadifMsg;
    else if (deint == "yadifdoubleprocessdeint")
        msg = kYadifMsg + " " +  kDoubleRateMsg;
    else if (deint == "fieldorderdoubleprocessdeint")
        msg = kFieldOrderMsg + " " +  kDoubleRateMsg;
    else if (deint == "opengldoublerateyadif")
        msg = kYadifMsg + " " +  kDoubleRateMsg + " " +  kUsingGPU;
    else if (deint == "vdpauonefield")
        msg = kOneFieldMsg + " " + kUsingGPU;
    else if (deint == "vdpaubobdeint")
        msg = kBobMsg + " " + kUsingGPU;
    else if (deint == "vdpaubasic")
        msg = kBasicMsg + " " + kUsingGPU;
    else if (deint == "vdpauadvanced")
        msg = kAdvMsg + " " + kUsingGPU;
    else if (deint == "vdpaubasicdoublerate")
        msg = kBasicMsg + " " +  kDoubleRateMsg + " " + kUsingGPU;
    else if (deint == "vdpauadvanceddoublerate")
        msg = kAdvMsg + " " +  kDoubleRateMsg + " " + kUsingGPU;
    else
        msg = QObject::tr("'%1' has not been documented yet.").arg(deint);

    return msg;
}

QStringList VideoDisplayProfile::GetOSDs(const QString &video_renderer)
{
    QMutexLocker locker(&safe_lock);
    init_statics();

    safe_map_t::const_iterator it = safe_osd.find(video_renderer);
    QStringList tmp;
    if (it != safe_osd.end())
        tmp = *it;

    tmp.detach();
    return tmp;
}

QString VideoDisplayProfile::GetOSDHelp(const QString &osd)
{

    QString msg = QObject::tr("OSD rendering method");

    if (osd.isEmpty())
        return msg;

    if (osd == "chromakey")
        msg = QObject::tr(
            "Render the OSD using the XVideo chromakey feature."
            "This renderer does not alpha blend. But it is the fastest "
            "OSD renderer; and is particularly efficient compared to the "
            "ia44blend OSD renderer for XvMC.") + "\n" +
            QObject::tr(
                "Note: nVidia hardware after the 5xxx series does not "
                "have XVideo chromakey support.");


    if (osd == "softblend")
    {
        msg = QObject::tr(
            "Software OSD rendering uses your CPU to alpha blend the OSD.");
    }

    if (osd == "ia44blend")
    {
        msg = QObject::tr(
            "Uses hardware support for 16 color alpha-blend surfaces for "
            "rendering the OSD. Because of the limited color range, MythTV "
            "renders the OSD in 16 level grayscale.") + "\n" +
            QObject::tr(
                "Note: Not recommended for nVidia or Intel chipsets. This "
                "removes two of the limited XvMC buffers from decoding duty.");
    }

    if (osd == "ivtv")
    {
        msg = QObject::tr(
            "Renders the OSD using the PVR-350 chromakey feature.");
    }

    if (osd.contains("opengl"))
    {
        msg = QObject::tr(
            "Uses OpenGL to alpha blend the OSD onto the video.");
    }

    return msg;
}

bool VideoDisplayProfile::IsFilterAllowed(const QString &video_renderer)
{
    QMutexLocker locker(&safe_lock);
    init_statics();
    return safe_custom.contains(video_renderer);
}

QStringList VideoDisplayProfile::GetFilteredRenderers(
    const QString &decoder, const QStringList &renderers)
{
    const QStringList dec_list = GetVideoRenderers(decoder);
    QStringList new_list;

    QStringList::const_iterator it = dec_list.begin();
    for (; it != dec_list.end(); ++it)
    {
        if (renderers.contains(*it))
            new_list.push_back(*it); // deep copy not needed
    }

    return new_list;
}

QString VideoDisplayProfile::GetBestVideoRenderer(const QStringList &renderers)
{
    QMutexLocker locker(&safe_lock);
    init_statics();

    uint    top_priority = 0;
    QString top_renderer = QString::null;

    QStringList::const_iterator it = renderers.begin();
    for (; it != renderers.end(); ++it)
    {
        priority_map_t::const_iterator p = safe_renderer_priority.find(*it);
        if ((p != safe_renderer_priority.end()) && (*p >= top_priority))
        {
            top_priority = *p;
            top_renderer = *it;
        }
    }

    if (!top_renderer.isNull())
        top_renderer.detach();

    return top_renderer;
}

QString VideoDisplayProfile::toString(void) const
{
    QString renderer  = GetPreference("pref_videorenderer");
    QString osd       = GetPreference("pref_osdrenderer");
    QString deint0    = GetPreference("pref_deint0");
    QString deint1    = GetPreference("pref_deint1");
    QString filter    = GetPreference("pref_filters");
    return QString("rend(%4) osd(%5) deint(%6,%7) filt(%8)")
        .arg(renderer).arg(osd).arg(deint0).arg(deint1).arg(filter);
}

/*
// Decoders
"dummy"
"nuppel"
"ffmpeg"
"libmpeg2"
"xvmc"
"xvmc-vld"
"macaccel"
"ivtv"
"vdpau"

// Video Renderers
"null"
"xlib"
"xshm"
"xv-blit"
"xvmc-blit"
"directfb"
"quartz-blit"
"quartz-accel"
"ivtv"
"opengl"
"vdpau"

// OSD Renderers
"chromakey"
"softblend"
"ia44blend"
"ivtv"
"opengl"
"opengl2"
"opengl3"
"vdpau"

// deinterlacers
"none"
"onefield"
"bobdeint"
"linearblend"
"kerneldeint"
"kerneldoubleprocessdeint"
"greedyhdeint"
"greedyhdoubleprocessdeint"
"yadifdeint"
"yadifdoubleprocessdeint"
"fieldorderdoubleprocessdeint"
"opengllinearblend"
"openglkerneldeint"
"openglonefield"
"openglbobdeint"
"openglyadif"
"opengldoubleratelinearblend"
"opengldoubleratekerneldeint"
"opengldoubleratefieldorder"
"opengldoublerateyadif"
"vdpauonefield"
"vdpaubobdeint"
"vdpaubasic"
"vdpauadvanced"
"vdpaubasicdoublerate"
"vdpauadvanceddoublerate"
*/

void VideoDisplayProfile::init_statics(void)
{
    if (safe_initialized)
        return;

    safe_initialized = true;

    safe_custom += "null";
    safe_custom += "xlib";
    safe_custom += "xshm";
    safe_custom += "directfb";
    safe_custom += "direct3d";
    safe_custom += "quartz-blit";
    safe_custom += "xv-blit";
    safe_custom += "opengl";

    safe_list_t::const_iterator it;
    for (it = safe_custom.begin(); it != safe_custom.end(); ++it)
    {
        safe_deint[*it] += "onefield";
        safe_deint[*it] += "linearblend";
        safe_deint[*it] += "kerneldeint";
        safe_deint[*it] += "kerneldoubleprocessdeint";
        safe_deint[*it] += "greedyhdeint";
        safe_deint[*it] += "greedyhdoubleprocessdeint";
        safe_deint[*it] += "yadifdeint";
        safe_deint[*it] += "yadifdoubleprocessdeint";
        safe_deint[*it] += "fieldorderdoubleprocessdeint";
        safe_deint[*it] += "none";
        safe_osd[*it]   += "softblend";
    }

    // allow vdpau filters
    safe_custom += "vdpau";

    safe_deint["xv-blit"]   += "bobdeint";
    safe_deint["xvmc-blit"] += "bobdeint";
    safe_deint["xvmc-blit"] += "onefield";
    safe_deint["xvmc-blit"] += "none";
    safe_osd["xv-blit"]     += "chromakey";
    safe_osd["xvmc-blit"]   += "chromakey";

    safe_deint["opengl"] += "opengllinearblend";
    safe_deint["opengl"] += "openglonefield";
    safe_deint["opengl"] += "openglkerneldeint";
    safe_deint["opengl"] += "bobdeint";
    safe_deint["opengl"] += "openglbobdeint";
    safe_deint["opengl"] += "opengldoubleratelinearblend";
    safe_deint["opengl"] += "opengldoubleratekerneldeint";
    safe_deint["opengl"] += "opengldoubleratefieldorder";
    safe_deint["opengl"] += "opengldoublerateyadif";
    safe_deint["opengl"] += "openglyadif";

    safe_deint["vdpau"] += "none";
    safe_deint["vdpau"] += "vdpauonefield";
    safe_deint["vdpau"] += "vdpaubobdeint";
    safe_deint["vdpau"] += "vdpaubasic";
    safe_deint["vdpau"] += "vdpauadvanced";
    safe_deint["vdpau"] += "vdpaubasicdoublerate";
    safe_deint["vdpau"] += "vdpauadvanceddoublerate";

    safe_osd["xv-blit"]     += "softblend";
    safe_osd["xvmc-blit"]   += "chromakey";
    safe_osd["xvmc-blit"]   += "ia44blend";
    safe_osd["ivtv"]        += "ivtv";
    safe_osd["opengl"]      += "opengl2";
    safe_osd["quartz-accel"]+= "opengl3";
    safe_osd["vdpau"]       += "vdpau";
    safe_osd["direct3d"]    += "direct3d";

    // These video renderers do not support deinterlacing in MythTV
    safe_deint["quartz-accel"] += "none";
    safe_deint["ivtv"]         += "none";

    QStringList tmp;
    tmp += "dummy";
    tmp += "nuppel";
    tmp += "ffmpeg";
    tmp += "libmpeg2";
    QStringList::const_iterator it2;
    for (it2 = tmp.begin(); it2 != tmp.end(); ++it2)
    {
        safe_renderer[*it2] += "null";
        safe_renderer[*it2] += "xlib";
        safe_renderer[*it2] += "xshm";
        safe_renderer[*it2] += "directfb";
        safe_renderer[*it2] += "direct3d";
        safe_renderer[*it2] += "quartz-blit";
        safe_renderer[*it2] += "xv-blit";
        safe_renderer[*it2] += "opengl";
        safe_renderer[*it2] += "vdpau";
    }

    safe_renderer["dummy"]    += "xvmc-blit";
    safe_renderer["xvmc"]     += "xvmc-blit";
    safe_renderer["xvmc-vld"] += "xvmc-blit";

    safe_renderer["dummy"]    += "quartz-accel";
    safe_renderer["macaccel"] += "quartz-accel";
    safe_renderer["ivtv"]     += "ivtv";
    safe_renderer["vdpau"]    += "vdpau";

    safe_renderer_priority["null"]         =  10;
    safe_renderer_priority["xlib"]         =  20;
    safe_renderer_priority["xshm"]         =  30;
    safe_renderer_priority["xv-blit"]      =  90;
    safe_renderer_priority["xvmc-blit"]    = 110;
    safe_renderer_priority["vdpau"]        = 120;
    safe_renderer_priority["directfb"]     =  60;
    safe_renderer_priority["direct3d"]     =  55;
    safe_renderer_priority["quartz-blit"]  =  70;
    safe_renderer_priority["quartz-accel"] =  80;
    safe_renderer_priority["ivtv"]         =  40;

    safe_equiv_dec["ffmpeg"]   += "nuppel";
    safe_equiv_dec["libmpeg2"] += "nuppel";
    safe_equiv_dec["libmpeg2"] += "ffmpeg";

    safe_equiv_dec["ffmpeg"]   += "dummy";
    safe_equiv_dec["libmpeg2"] += "dummy";
    safe_equiv_dec["xvmc"]     += "dummy";
    safe_equiv_dec["xvmc-vld"] += "dummy";
    safe_equiv_dec["macaccel"] += "dummy";
    safe_equiv_dec["ivtv"]     += "dummy";
    safe_equiv_dec["vdpau"]    += "dummy";

    safe_renderer_group["x11"] += "xlib";
    safe_renderer_group["x11"] += "xshm";
    safe_renderer_group["x11"] += "xv-blit";
    safe_renderer_group["x11"] += "xvmc-blit";
    safe_renderer_group["quartz"] += "quartz-blit";
    safe_renderer_group["quartz"] += "quartz-accel";
}
