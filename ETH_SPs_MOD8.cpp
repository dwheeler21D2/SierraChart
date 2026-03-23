#include "sierrachart.h"
#include <vector>
#include <cmath>
#include <algorithm>

SCDLLName("ETH_SinglePrints_30m_Excess_And_Internal")

static inline float Min2(float a, float b) { return a < b ? a : b; }
static inline float Max2(float a, float b) { return a > b ? a : b; }

static int TimeToSeconds(const SCDateTime& dt)
{
    return dt.GetHour() * 3600 + dt.GetMinute() * 60 + dt.GetSecond();
}

// 1 = RTH if in window, else 2 = ETH
static int GetSessionType_RTHorETH(const SCDateTime& dt, int rthStartSec, int rthEndSec)
{
    const int t = TimeToSeconds(dt);
    if (rthStartSec < rthEndSec)
    {
        if (t >= rthStartSec && t <= rthEndSec)
            return 1;
    }
    return 2;
}

// ETH anchor date = evening date when session crosses midnight
static int GetSessionAnchorDate(const SCDateTime& dt, int sessionType, int ethStartSec, int ethEndSec)
{
    const int date = dt.GetDate();
    if (sessionType == 1)
        return date;

    const int t = TimeToSeconds(dt);
    if (ethStartSec > ethEndSec)
    {
        if (t <= ethEndSec)
            return date - 1;
    }
    return date;
}

static long long GetCurrentBarSessionID(const SCDateTime& dt, int sessionType, int ethStartSec, int ethEndSec)
{
    if (sessionType == 1)
        return (long long)dt.GetDate() * 10 + 1;

    const int anchorDate = GetSessionAnchorDate(dt, sessionType, ethStartSec, ethEndSec);
    return (long long)anchorDate * 10 + 2;
}

// 30m bucket aligned to :00/:30 based on timestamp
static long long Get30mKey(const SCDateTime& dt)
{
    const int hh = dt.GetHour();
    const int mm = dt.GetMinute();
    const int half = (mm >= 30) ? 1 : 0;
    const int slot = hh * 2 + half;
    const int date = dt.GetDate();
    return (long long)date * 100 + slot;
}

// SessionType encoded in SessionID = anchorDate*10 + sessionType
static int GetZoneSessionTypeFromID(long long originSessionID)
{
    const int st = (int)(originSessionID % 10);
    return (st == 1 || st == 2) ? st : 0;
}

static bool HasMinimumTimeSpanMinutes(
    SCStudyInterfaceRef sc,
    int startIndex,
    int endIndex,
    int minMinutes)
{
    if (startIndex < 0 || endIndex < startIndex || endIndex >= sc.ArraySize)
        return false;

    const SCDateTime startDT = sc.BaseDateTimeIn[startIndex];
    const SCDateTime endDT   = sc.BaseDateTimeIn[endIndex];
    const double minutes = (endDT - startDT).GetAsDouble() * 24.0 * 60.0;

    return minutes >= (double)minMinutes;
}

struct Period30m
{
    long long Key = 0;
    long long SessionID = 0;
    int SessionType = 0; // 1=RTH, 2=ETH
    int StartIndex = -1;
    int EndIndex = -1;
    float High = 0.0f;
    float Low  = 0.0f;
    bool Closed = false;
};

enum ZoneType
{
    ZT_EXCESS_TOP = 1,
    ZT_EXCESS_BOTTOM = 2,
    ZT_INTERNAL = 3
};

struct Zone
{
    int DrawingID = 0;
    long long OriginSessionID = 0;
    ZoneType Type = ZT_INTERNAL;

    int StartIndex = 0;
    int EndIndex = -1;            // -1 means extend right
    int FirstMitigateIndex = 0;   // mitigation begins here

    float Top = 0.0f;
    float Bottom = 0.0f;

    bool Active = true;
    bool Frozen = false;
    bool RebuildManaged = false;
    bool HadIntraSessionMitigation = false;
};

static void DrawZoneRect(SCStudyInterfaceRef sc, const Zone& z,
                         COLORREF fillColor, COLORREF borderColor, int transparency)
{
    s_UseTool tool;
    tool.Clear();

    tool.ChartNumber = sc.ChartNumber;
    tool.Region = 0;

    const bool isTruncated = (z.EndIndex >= 0) || z.Frozen;

    tool.DrawingType = isTruncated ? DRAWING_RECTANGLEHIGHLIGHT
                                   : DRAWING_RECTANGLE_EXT_HIGHLIGHT;

    tool.LineNumber = z.DrawingID;
    tool.AddAsUserDrawnDrawing = 1;

    tool.BeginIndex = z.StartIndex;
    tool.EndIndex   = isTruncated ? z.EndIndex : sc.ArraySize - 1;

    tool.BeginValue = z.Top;
    tool.EndValue   = z.Bottom;

    tool.Color = borderColor;
    tool.LineWidth = 1;

    if (z.Type == ZT_EXCESS_TOP || z.Type == ZT_EXCESS_BOTTOM)
        tool.LineStyle = LINESTYLE_DASH;
    else
        tool.LineStyle = LINESTYLE_SOLID;

    tool.SecondaryColor = fillColor;
    tool.TransparencyLevel = transparency;
    tool.AddMethod = UTAM_ADD_OR_ADJUST;
    tool.AllowCopyToOtherCharts = 1;

    sc.UseTool(tool);
}

static void DeleteZoneDrawing(SCStudyInterfaceRef sc, const Zone& z)
{
    sc.DeleteUserDrawnACSDrawing(sc.ChartNumber, z.DrawingID);
}

static void AppendSuccessorZone(
    const Zone& src,
    float newBottom,
    float newTop,
    int startIndex,
    int firstMitigateIndex,
    float minHeight,
    int& nextID,
    std::vector<Zone>& outZones)
{
    if ((newTop - newBottom) < minHeight)
        return;

    Zone nz;
    nz.DrawingID = nextID++;
    nz.OriginSessionID = src.OriginSessionID;
    nz.Type = src.Type;
    nz.StartIndex = startIndex;
    nz.EndIndex = -1;
    nz.FirstMitigateIndex = firstMitigateIndex;
    nz.Top = newTop;
    nz.Bottom = newBottom;
    nz.Active = true;
    nz.Frozen = false;
    nz.RebuildManaged = false;
    nz.HadIntraSessionMitigation = src.HadIntraSessionMitigation;

    outZones.push_back(nz);
}

static void MitigateOneBar_PartialAware(
    SCStudyInterfaceRef sc,
    Zone& z,
    int barIndex,
    float barL, float barH,
    float eps,
    float minHeight,
    bool eraseOnMitigation,
    bool showMitigatedIntraSessionZones,
    long long currentBarSessionID,
    int minFrozenFragmentMinutes,
    int& nextID,
    std::vector<Zone>& outZones)
{
    if (!z.Active)
        return;

    if (z.Frozen)
    {
        outZones.push_back(z);
        return;
    }

    if (barIndex < z.FirstMitigateIndex)
    {
        outZones.push_back(z);
        return;
    }

    if (barH <= z.Bottom + eps || barL >= z.Top - eps)
    {
        outZones.push_back(z);
        return;
    }

    const bool intraSessionMitigation = (currentBarSessionID == z.OriginSessionID);
    const bool suppressFrozenHistorical =
        (!showMitigatedIntraSessionZones && intraSessionMitigation);

    if (eraseOnMitigation)
    {
        DeleteZoneDrawing(sc, z);
        return;
    }

    const float ovL = Max2(z.Bottom, barL);
    const float ovH = Min2(z.Top,    barH);

    if (suppressFrozenHistorical)
    {
        DeleteZoneDrawing(sc, z);
    }
    else
    {
        Zone frozenHist = z;
        frozenHist.EndIndex = barIndex - 1;
        frozenHist.Frozen = true;
        frozenHist.HadIntraSessionMitigation =
            z.HadIntraSessionMitigation || intraSessionMitigation;

        const bool validSpan =
            (frozenHist.EndIndex >= frozenHist.StartIndex);

        const bool keepFrozenHist =
            validSpan &&
            HasMinimumTimeSpanMinutes(
                sc,
                frozenHist.StartIndex,
                frozenHist.EndIndex,
                minFrozenFragmentMinutes
            );

        if (keepFrozenHist)
            outZones.push_back(frozenHist);
        else
            DeleteZoneDrawing(sc, z);
    }

    if (ovL > z.Bottom + eps)
    {
        Zone temp = z;
        temp.HadIntraSessionMitigation =
            z.HadIntraSessionMitigation || intraSessionMitigation;

        if (intraSessionMitigation &&
            (temp.Type == ZT_EXCESS_TOP || temp.Type == ZT_EXCESS_BOTTOM))
        {
            temp.Type = ZT_INTERNAL;
        }

        AppendSuccessorZone(
            temp,
            z.Bottom,
            ovL,
            barIndex,
            barIndex + 1,
            minHeight,
            nextID,
            outZones
        );
    }

    if (z.Top > ovH + eps)
    {
        Zone temp = z;
        temp.HadIntraSessionMitigation =
            z.HadIntraSessionMitigation || intraSessionMitigation;

        if (intraSessionMitigation &&
            (temp.Type == ZT_EXCESS_TOP || temp.Type == ZT_EXCESS_BOTTOM))
        {
            temp.Type = ZT_INTERNAL;
        }

        AppendSuccessorZone(
            temp,
            ovH,
            z.Top,
            barIndex,
            barIndex + 1,
            minHeight,
            nextID,
            outZones
        );
    }
}

static void ReplayHistoricalMitigationForNewZone(
    SCStudyInterfaceRef sc,
    Zone& seedZone,
    int currentIndex,
    int rthStartSec,
    int rthEndSec,
    int ethStartSec,
    int ethEndSec,
    bool mitigateOnlyLikeSessions,
    bool eraseOnMitigation,
    bool showMitigatedIntraSessionZones,
    float eps,
    float minHeight,
    int minFrozenFragmentMinutes,
    int& nextID,
    std::vector<Zone>& outZones)
{
    std::vector<Zone> working;
    working.push_back(seedZone);

    for (int j = seedZone.FirstMitigateIndex; j < currentIndex; ++j)
    {
        if (working.empty())
            break;

        const SCDateTime dtj = sc.BaseDateTimeIn[j];
        const int barSessionType = GetSessionType_RTHorETH(dtj, rthStartSec, rthEndSec);
        const long long barSessionID = GetCurrentBarSessionID(dtj, barSessionType, ethStartSec, ethEndSec);

        const float barL = sc.Low[j];
        const float barH = sc.High[j];

        std::vector<Zone> nextWorking;
        nextWorking.reserve(working.size() + 4);

        for (auto& wz : working)
        {
            if (mitigateOnlyLikeSessions)
            {
                const int zoneSessionType = GetZoneSessionTypeFromID(wz.OriginSessionID);
                if (zoneSessionType != 0 && zoneSessionType != barSessionType)
                {
                    nextWorking.push_back(wz);
                    continue;
                }
            }

            MitigateOneBar_PartialAware(
                sc,
                wz,
                j,
                barL,
                barH,
                eps,
                minHeight,
                eraseOnMitigation,
                showMitigatedIntraSessionZones,
                barSessionID,
                minFrozenFragmentMinutes,
                nextID,
                nextWorking
            );
        }

        working.swap(nextWorking);
    }

    for (auto& z : working)
        outZones.push_back(z);
}

static void RemoveZonesForSessionAndType(
    SCStudyInterfaceRef sc,
    std::vector<Zone>& zones,
    long long sessionID,
    ZoneType typeToRemove)
{
    for (auto& z : zones)
    {
        if (z.Active &&
            z.OriginSessionID == sessionID &&
            z.Type == typeToRemove &&
            z.RebuildManaged &&
            !z.Frozen)
        {
            DeleteZoneDrawing(sc, z);
            z.Active = false;
        }
    }

    std::vector<Zone> kept;
    kept.reserve(zones.size());
    for (auto& z : zones)
        if (z.Active)
            kept.push_back(z);
    zones.swap(kept);
}

static void RemoveMitigatedIntraSessionZonesForCompletedSession(
    SCStudyInterfaceRef sc,
    std::vector<Zone>& zones,
    long long completedSessionID)
{
    for (auto& z : zones)
    {
        if (z.Active &&
            z.OriginSessionID == completedSessionID &&
            z.HadIntraSessionMitigation)
        {
            DeleteZoneDrawing(sc, z);
            z.Active = false;
        }
    }

    std::vector<Zone> kept;
    kept.reserve(zones.size());
    for (auto& z : zones)
        if (z.Active)
            kept.push_back(z);
    zones.swap(kept);
}

static void RemoveOverlappingInternalZonesInSession(
    SCStudyInterfaceRef sc,
    std::vector<Zone>& zones,
    long long sessionID,
    float L,
    float H,
    float eps)
{
    for (auto& z : zones)
    {
        if (!z.Active) continue;
        if (z.OriginSessionID != sessionID) continue;
        if (z.Type != ZT_INTERNAL) continue;

        if (!(z.Top <= L + eps || z.Bottom >= H - eps))
        {
            DeleteZoneDrawing(sc, z);
            z.Active = false;
        }
    }

    std::vector<Zone> kept;
    kept.reserve(zones.size());
    for (auto& z : zones)
        if (z.Active)
            kept.push_back(z);
    zones.swap(kept);
}

struct AtomicBand
{
    float L = 0.0f;
    float H = 0.0f;
    int OwnerSessPos = -1; // position within session-period list
    ZoneType Type = ZT_INTERNAL;
};

static void RebuildSessionZones_FromAtomicBands(
    SCStudyInterfaceRef sc,
    const std::vector<Period30m>& periods,
    std::vector<Zone>& zones,
    long long sessionID,
    int sessionType,
    float eps,
    float minHeight,
    int& nextID,
    int currentIndex,
    int rthStartSec,
    int rthEndSec,
    int ethStartSec,
    int ethEndSec,
    bool mitigateOnlyLikeSessions,
    bool eraseOnMitigation,
    bool showMitigatedIntraSessionZones,
    int minFrozenFragmentMinutes)
{
    RemoveZonesForSessionAndType(sc, zones, sessionID, ZT_INTERNAL);
    RemoveZonesForSessionAndType(sc, zones, sessionID, ZT_EXCESS_TOP);
    RemoveZonesForSessionAndType(sc, zones, sessionID, ZT_EXCESS_BOTTOM);

    std::vector<const Period30m*> sess;
    sess.reserve(128);

    for (const auto& p : periods)
    {
        if (p.Closed && p.SessionID == sessionID && p.SessionType == sessionType)
            sess.push_back(&p);
    }

    if (sess.empty())
        return;

    float sessionHigh = -1e30f;
    float sessionLow  =  1e30f;

    std::vector<float> bounds;
    bounds.reserve(sess.size() * 2);

    for (const auto* p : sess)
    {
        bounds.push_back(p->Low);
        bounds.push_back(p->High);
        if (p->High > sessionHigh) sessionHigh = p->High;
        if (p->Low  < sessionLow)  sessionLow  = p->Low;
    }

    std::sort(bounds.begin(), bounds.end());
    std::vector<float> uniq;
    uniq.reserve(bounds.size());

    for (float v : bounds)
    {
        if (uniq.empty() || fabsf(v - uniq.back()) > eps)
            uniq.push_back(v);
    }

    if (uniq.size() < 2)
        return;

    std::vector<AtomicBand> bands;
    bands.reserve(uniq.size());

    for (size_t i = 0; i + 1 < uniq.size(); ++i)
    {
        const float L = uniq[i];
        const float H = uniq[i + 1];

        if (H <= L + eps)
            continue;

        int count = 0;
        int owner = -1;

        for (int s = 0; s < (int)sess.size(); ++s)
        {
            const Period30m& p = *sess[s];
            const bool covers = (p.Low < H - eps) && (p.High > L + eps);
            if (covers)
            {
                ++count;
                owner = s;
                if (count > 1)
                    break;
            }
        }

        if (count == 1)
        {
            AtomicBand b;
            b.L = L;
            b.H = H;
            b.OwnerSessPos = owner;

            const bool touchesTop = fabsf(H - sessionHigh) <= eps;
            const bool touchesBottom = fabsf(L - sessionLow) <= eps;

            if (touchesTop)
                b.Type = ZT_EXCESS_TOP;
            else if (touchesBottom)
                b.Type = ZT_EXCESS_BOTTOM;
            else
                b.Type = ZT_INTERNAL;

            bands.push_back(b);
        }
    }

    if (bands.empty())
        return;

    std::vector<AtomicBand> merged;
    merged.reserve(bands.size());
    merged.push_back(bands[0]);

    for (size_t i = 1; i < bands.size(); ++i)
    {
        AtomicBand& prev = merged.back();
        const AtomicBand& cur = bands[i];

        if (cur.OwnerSessPos == prev.OwnerSessPos &&
            cur.Type == prev.Type &&
            fabsf(cur.L - prev.H) <= eps)
        {
            prev.H = cur.H;
        }
        else
        {
            merged.push_back(cur);
        }
    }

    for (const auto& b : merged)
    {
        if ((b.H - b.L) < minHeight)
            continue;

        const Period30m& ownerP = *sess[b.OwnerSessPos];

        if (b.Type == ZT_EXCESS_TOP || b.Type == ZT_EXCESS_BOTTOM)
        {
            RemoveOverlappingInternalZonesInSession(
                sc,
                zones,
                sessionID,
                b.L,
                b.H,
                eps
            );
        }

        Zone z;
        z.DrawingID = nextID++;
        z.OriginSessionID = sessionID;
        z.Type = b.Type;
        z.StartIndex = ownerP.StartIndex;
        z.FirstMitigateIndex = ownerP.EndIndex + 1;
        z.Top = b.H;
        z.Bottom = b.L;
        z.Active = true;
        z.Frozen = false;
        z.RebuildManaged = true;
        z.HadIntraSessionMitigation = false;

        ReplayHistoricalMitigationForNewZone(
            sc,
            z,
            currentIndex,
            rthStartSec,
            rthEndSec,
            ethStartSec,
            ethEndSec,
            mitigateOnlyLikeSessions,
            eraseOnMitigation,
            showMitigatedIntraSessionZones,
            eps,
            minHeight,
            minFrozenFragmentMinutes,
            nextID,
            zones
        );
    }
}

SCSFExport scsf_ETH_SinglePrints_30m_Excess_And_Internal(SCStudyInterfaceRef sc)
{
    SCInputRef In_RTH_Start   = sc.Input[1];
    SCInputRef In_RTH_End     = sc.Input[2];
    SCInputRef In_ETH_Start   = sc.Input[3];
    SCInputRef In_ETH_End     = sc.Input[4];

    SCInputRef In_DrawExcess   = sc.Input[5];
    SCInputRef In_DrawInternal = sc.Input[6];

    SCInputRef In_MinZoneTicks = sc.Input[7];

    SCInputRef In_FillColor    = sc.Input[8];
    SCInputRef In_BorderColor  = sc.Input[9];
    SCInputRef In_Transparency = sc.Input[11];

    SCInputRef In_EraseOnMitigation = sc.Input[12];
    SCInputRef In_MitigateOnlyLikeSessions = sc.Input[13];
    SCInputRef In_ShowMitigatedIntraSessionZones = sc.Input[14];
    SCInputRef In_MinFrozenFragmentMinutes = sc.Input[15];

    if (sc.SetDefaults)
    {
        sc.GraphName = "ETH 30m Single Prints (Atomic Band Logic)";
        sc.AutoLoop = 1;
        sc.GraphRegion = 0;

        In_RTH_Start.Name = "RTH Start (sec)";
        In_RTH_Start.SetInt(9*3600 + 30*60);

        In_RTH_End.Name = "RTH End (sec)";
        In_RTH_End.SetInt(17*3600);

        In_ETH_Start.Name = "ETH Start (sec)";
        In_ETH_Start.SetInt(18*3600);

        In_ETH_End.Name = "ETH End (sec)";
        In_ETH_End.SetInt(9*3600 + 29*60 + 59);

        In_DrawExcess.Name = "Draw Excess (Top/Bottom)";
        In_DrawExcess.SetYesNo(1);

        In_DrawInternal.Name = "Draw Internal Single Prints (Non-Excess)";
        In_DrawInternal.SetYesNo(1);

        In_MinZoneTicks.Name = "Minimum Zone Height (ticks)";
        In_MinZoneTicks.SetInt(4);

        In_FillColor.Name = "Fill Color";
        In_FillColor.SetColor(RGB(200, 200, 200));

        In_BorderColor.Name = "Border Color";
        In_BorderColor.SetColor(RGB(255, 0, 0));

        In_Transparency.Name = "Transparency (0-100)";
        In_Transparency.SetInt(75);

        In_EraseOnMitigation.Name = "Erase Zone On Mitigation? (No=Truncate/Shrink)";
        In_EraseOnMitigation.SetYesNo(0);

        In_MitigateOnlyLikeSessions.Name = "Mitigate Only In Like Sessions? (1=Yes,0=Any)";
        In_MitigateOnlyLikeSessions.SetYesNo(1);

        In_ShowMitigatedIntraSessionZones.Name = "Show Mitigated Intra-Session Single Print Zones?";
        In_ShowMitigatedIntraSessionZones.SetYesNo(0);

        In_MinFrozenFragmentMinutes.Name = "Minimum Frozen Fragment Width (minutes)";
        In_MinFrozenFragmentMinutes.SetInt(60);

        return;
    }

    auto* periods = (std::vector<Period30m>*)sc.GetPersistentPointer(1);
    if (!periods)
    {
        periods = new std::vector<Period30m>();
        sc.SetPersistentPointer(1, periods);
    }

    auto* zones = (std::vector<Zone>*)sc.GetPersistentPointer(2);
    if (!zones)
    {
        zones = new std::vector<Zone>();
        sc.SetPersistentPointer(2, zones);
    }

    int& nextID = sc.GetPersistentInt(1);
    if (nextID == 0)
        nextID = 10000;

    const int i = sc.Index;

    if (i == 0 && sc.UpdateStartIndex == 0)
    {
        for (auto& z : *zones)
            DeleteZoneDrawing(sc, z);

        zones->clear();
        periods->clear();
        nextID = 10000;
    }

    if (i == sc.ArraySize - 1)
        return;

    const int rthStart = In_RTH_Start.GetInt();
    const int rthEnd   = In_RTH_End.GetInt();
    const int ethStart = In_ETH_Start.GetInt();
    const int ethEnd   = In_ETH_End.GetInt();

    const float eps = sc.TickSize * 0.25f;
    const float minHeight = (float)In_MinZoneTicks.GetInt() * sc.TickSize;

    const bool eraseOnMitigation = (In_EraseOnMitigation.GetYesNo() != 0);
    const bool mitigateOnlyLikeSessions = (In_MitigateOnlyLikeSessions.GetYesNo() != 0);
    const bool showMitigatedIntraSessionZones = (In_ShowMitigatedIntraSessionZones.GetYesNo() != 0);
    const int minFrozenFragmentMinutes = In_MinFrozenFragmentMinutes.GetInt();

    const SCDateTime dt = sc.BaseDateTimeIn[i];
    const int sessionType = GetSessionType_RTHorETH(dt, rthStart, rthEnd);
    const long long currentBarSessionID = GetCurrentBarSessionID(dt, sessionType, ethStart, ethEnd);

    // Finalize last ETH 30m period when leaving ETH
    if (i > 0 && !periods->empty())
    {
        Period30m& last = periods->back();

        if (!last.Closed && last.SessionType == 2 && sessionType != 2)
        {
            last.EndIndex = i - 1;
            last.Closed = true;

            RebuildSessionZones_FromAtomicBands(
                sc,
                *periods,
                *zones,
                last.SessionID,
                last.SessionType,
                eps,
                minHeight,
                nextID,
                i,
                rthStart,
                rthEnd,
                ethStart,
                ethEnd,
                mitigateOnlyLikeSessions,
                eraseOnMitigation,
                showMitigatedIntraSessionZones,
                minFrozenFragmentMinutes
            );

            if (!In_ShowMitigatedIntraSessionZones.GetYesNo())
            {
                RemoveMitigatedIntraSessionZonesForCompletedSession(
                    sc,
                    *zones,
                    last.SessionID
                );
            }
        }
    }

    const bool isETH = (sessionType == 2);

    if (isETH)
    {
        const int anchorDate = GetSessionAnchorDate(dt, sessionType, ethStart, ethEnd);
        const long long sessionID = (long long)anchorDate * 10 + sessionType;
        const long long key = Get30mKey(dt);

        if (periods->empty() || periods->back().Key != key)
        {
            if (!periods->empty())
            {
                periods->back().EndIndex = i - 1;
                periods->back().Closed = true;
            }

            Period30m p;
            p.Key = key;
            p.SessionID = sessionID;
            p.SessionType = sessionType;
            p.StartIndex = i;
            p.EndIndex = i;
            p.High = sc.High[i];
            p.Low  = sc.Low[i];
            p.Closed = false;

            periods->push_back(p);

            RebuildSessionZones_FromAtomicBands(
                sc,
                *periods,
                *zones,
                sessionID,
                sessionType,
                eps,
                minHeight,
                nextID,
                i,
                rthStart,
                rthEnd,
                ethStart,
                ethEnd,
                mitigateOnlyLikeSessions,
                eraseOnMitigation,
                showMitigatedIntraSessionZones,
                minFrozenFragmentMinutes
            );
        }
        else
        {
            Period30m& cur = periods->back();
            cur.High = Max2(cur.High, sc.High[i]);
            cur.Low  = Min2(cur.Low,  sc.Low[i]);
            cur.EndIndex = i;
        }
    }

    const float barL = sc.Low[i];
    const float barH = sc.High[i];

    std::vector<Zone> nextZones;
    nextZones.reserve(zones->size() + 32);

    for (auto& z : *zones)
    {
        if (mitigateOnlyLikeSessions)
        {
            const int zoneSessionType = GetZoneSessionTypeFromID(z.OriginSessionID);
            if (zoneSessionType != 0 && sessionType != zoneSessionType)
            {
                nextZones.push_back(z);
                continue;
            }
        }

        MitigateOneBar_PartialAware(
            sc,
            z,
            i,
            barL,
            barH,
            eps,
            minHeight,
            eraseOnMitigation,
            showMitigatedIntraSessionZones,
            currentBarSessionID,
            minFrozenFragmentMinutes,
            nextID,
            nextZones
        );
    }

    zones->swap(nextZones);

    const COLORREF fillColor = In_FillColor.GetColor();
    const COLORREF borderColor = In_BorderColor.GetColor();
    const int transparency = In_Transparency.GetInt();

    for (const auto& z : *zones)
    {
        if (z.Active)
            DrawZoneRect(sc, z, fillColor, borderColor, transparency);
    }
}