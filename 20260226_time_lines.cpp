#include "sierrachart.h"

SCDLLName("Time Anchored Rays (4 Items)")

// ---------- Helpers ----------
static int TimeToSecondsHHMM(int hhmm)
{
    int hh = hhmm / 100;
    int mm = hhmm % 100;
    if (hh < 0) hh = 0;
    if (hh > 23) hh = 23;
    if (mm < 0) mm = 0;
    if (mm > 59) mm = 59;
    return hh * 3600 + mm * 60;
}

static int GetTimeSeconds(const SCDateTime& dt)
{
    return dt.GetHour() * 3600 + dt.GetMinute() * 60 + dt.GetSecond();
}

static int ToYYYYMMDD(const SCDateTime& dt)
{
    const int y = dt.GetYear();
    const int m = dt.GetMonth();
    const int d = dt.GetDay();
    return y * 10000 + m * 100 + d;
}

static int MakeLineNumber(int studyInstanceID, int itemIndex /*1..4*/, int yyyymmdd)
{
    // Stable per-study-per-item-per-day line number
    return (studyInstanceID * 100000000) + (itemIndex * 10000000) + (yyyymmdd % 10000000);
}

static SubgraphLineStyles InputLineStyleToSC(int style)
{
    // 0 Solid, 1 Dash, 2 Dot, 3 DashDot, 4 DashDotDot
    switch (style)
    {
    default: return LINESTYLE_SOLID;
    case 1:  return LINESTYLE_DASH;
    case 2:  return LINESTYLE_DOT;
    case 3:  return LINESTYLE_DASHDOT;
    case 4:  return LINESTYLE_DASHDOTDOT;
    }
}

// Find bar index on a given calendar date whose start time is the first bar that contains targetSec:
// Date(i) == targetDate AND Time(i) <= targetSec < Time(i+1)
static int FindBarIndexForDateAndTime(SCStudyInterfaceRef sc, int targetYYYYMMDD, int targetSec)
{
    const int n = sc.ArraySize;
    if (n < 2)
        return -1;

    for (int i = 0; i < n - 1; ++i)
    {
        const SCDateTime& dt0 = sc.BaseDateTimeIn[i];
        const SCDateTime& dt1 = sc.BaseDateTimeIn[i + 1];

        const int d0 = ToYYYYMMDD(dt0);
        if (d0 != targetYYYYMMDD)
            continue;

        const int t0 = GetTimeSeconds(dt0);
        const int t1 = GetTimeSeconds(dt1);

        const int d1 = ToYYYYMMDD(dt1);

        if (d1 == d0)
        {
            if (t0 <= targetSec && targetSec < t1)
                return i;
        }
        else
        {
            // next bar is next date; treat bar i as spanning to end-of-day
            if (t0 <= targetSec)
                return i;
        }
    }

    // last bar check
    {
        const int i = n - 1;
        const SCDateTime& dt = sc.BaseDateTimeIn[i];
        if (ToYYYYMMDD(dt) == targetYYYYMMDD)
        {
            const int t = GetTimeSeconds(dt);
            if (t <= targetSec)
                return i;
        }
    }

    return -1;
}

// Collect up to lookbackDays distinct calendar dates from the chart, most recent first
static int CollectRecentDates(SCStudyInterfaceRef sc, int lookbackDays, int* outYYYYMMDD, int outCap)
{
    if (lookbackDays <= 0 || outCap <= 0)
        return 0;

    const int n = sc.ArraySize;
    int count = 0;
    int lastSeen = -1;

    for (int i = n - 1; i >= 0 && count < lookbackDays && count < outCap; --i)
    {
        const int d = ToYYYYMMDD(sc.BaseDateTimeIn[i]);
        if (d != lastSeen)
        {
            outYYYYMMDD[count++] = d;
            lastSeen = d;
        }
    }

    return count;
}

static void DeleteDrawingIfExists(SCStudyInterfaceRef sc, int lineNumber)
{
    // This is the correct ACSIL way to delete a drawing made by this study
    sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, lineNumber);
}

static void DrawRay(
    SCStudyInterfaceRef sc,
    int lineNumber,
    const SCDateTime& beginDT,
    float price,
    int lineWidth,
    SubgraphLineStyles lineStyle,
    COLORREF color)
{
    const int lastIndex = sc.ArraySize > 0 ? sc.ArraySize - 1 : 0;
    const SCDateTime& endDT = sc.BaseDateTimeIn[lastIndex];

    s_UseTool t;
    t.Clear();

    t.ChartNumber = sc.ChartNumber;
    t.DrawingType = DRAWING_LINE;
    t.Region = 0;
    t.AddMethod = UTAM_ADD_OR_ADJUST;
    t.LineNumber = lineNumber;

    t.BeginDateTime = beginDT;
    t.BeginValue = price;
    t.EndDateTime = endDT;
    t.EndValue = price;

    t.Color = color;
    t.LineWidth = lineWidth;
    t.LineStyle = lineStyle;

    sc.UseTool(t);
}

static float GetAnchorPriceForBar(SCStudyInterfaceRef sc, int index, int anchorMode)
{
    // 0=Open, 1=Close, 2=High, 3=Low
    switch (anchorMode)
    {
    default:
    case 0: return sc.Open[index];
    case 1: return sc.Close[index];
    case 2: return sc.High[index];
    case 3: return sc.Low[index];
    }
}

// ---------- Study ----------
SCSFExport scsf_TimeAnchoredRays_4Items(SCStudyInterfaceRef sc)
{
    SCInputRef In_Enable = sc.Input[0];

    // Per-item inputs block (7 each)
    // [Enable, TimeHHMM, Anchor(0=Open,1=Close,2=High,3=Low), LookbackDays, LineWidth, LineStyle, Color]
    auto ItemBase = [](int itemIndex) -> int { return 1 + (itemIndex - 1) * 7; };

    if (sc.SetDefaults)
    {
        sc.GraphName = "Time Anchored Rays (4 Items)";
        sc.StudyDescription = "Draw up to 4 sets of horizontal rays starting at a specified time and anchored to that bar's Open/Close/High/Low, for the last N days.";
        sc.AutoLoop = 0;
        sc.GraphRegion = 0;

        In_Enable.Name = "Enable Study";
        In_Enable.SetYesNo(1);

        for (int item = 1; item <= 4; ++item)
        {
            const int b = ItemBase(item);
            SCString s;

            s.Format("Item %d: Enable", item);
            sc.Input[b + 0].Name = s.GetChars();
            sc.Input[b + 0].SetYesNo(item == 1 ? 1 : 0);

            s.Format("Item %d: Start Time (HHMM, e.g. 1800, 0930)", item);
            sc.Input[b + 1].Name = s.GetChars();
            sc.Input[b + 1].SetInt(item == 1 ? 1800 : (item == 2 ? 930 : 0));
            sc.Input[b + 1].SetIntLimits(0, 2359);

            s.Format("Item %d: Anchor Price (0=Open, 1=Close, 2=High, 3=Low)", item);
            sc.Input[b + 2].Name = s.GetChars();
            sc.Input[b + 2].SetInt(1);
            sc.Input[b + 2].SetIntLimits(0, 3);

            s.Format("Item %d: Lookback Days (number of lines)", item);
            sc.Input[b + 3].Name = s.GetChars();
            sc.Input[b + 3].SetInt(item == 1 ? 1 : 3);
            sc.Input[b + 3].SetIntLimits(0, 60);

            s.Format("Item %d: Line Width", item);
            sc.Input[b + 4].Name = s.GetChars();
            sc.Input[b + 4].SetInt(2);
            sc.Input[b + 4].SetIntLimits(1, 10);

            s.Format("Item %d: Line Style (0=Solid,1=Dash,2=Dot,3=DashDot,4=DashDotDot)", item);
            sc.Input[b + 5].Name = s.GetChars();
            sc.Input[b + 5].SetInt(0);
            sc.Input[b + 5].SetIntLimits(0, 4);

            s.Format("Item %d: Color", item);
            sc.Input[b + 6].Name = s.GetChars();

            if (item == 1) sc.Input[b + 6].SetColor(RGB(255, 215, 0));
            if (item == 2) sc.Input[b + 6].SetColor(RGB(0, 200, 255));
            if (item == 3) sc.Input[b + 6].SetColor(RGB(255, 120, 120));
            if (item == 4) sc.Input[b + 6].SetColor(RGB(180, 180, 180));
        }

        return;
    }

    if (!In_Enable.GetYesNo() || sc.ArraySize < 2)
        return;

    const int studyInstanceID = sc.StudyGraphInstanceID;

    // manage up to 60 recent distinct dates
    int recentDates[60];
    const int recentCount = CollectRecentDates(sc, 60, recentDates, 60);

    for (int item = 1; item <= 4; ++item)
    {
        const int b = ItemBase(item);

        const bool itemEnable = sc.Input[b + 0].GetYesNo() != 0;
        const int timeHHMM = sc.Input[b + 1].GetInt();
        const int anchorMode = sc.Input[b + 2].GetInt(); // 0=open,1=close,2=high,3=low
        const int lookbackDays = sc.Input[b + 3].GetInt();
        const int lineWidth = sc.Input[b + 4].GetInt();
        const SubgraphLineStyles lineStyle = InputLineStyleToSC(sc.Input[b + 5].GetInt());
        const COLORREF color = sc.Input[b + 6].GetColor();

        const int targetSec = TimeToSecondsHHMM(timeHHMM);

        // delete anything beyond lookback or when disabled
        const int maxToManage = (recentCount < 60 ? recentCount : 60);

        for (int k = 0; k < maxToManage; ++k)
        {
            const int yyyymmdd = recentDates[k];
            const int ln = MakeLineNumber(studyInstanceID, item, yyyymmdd);

            const bool shouldDraw = itemEnable && (k < lookbackDays);

            if (!shouldDraw)
            {
                DeleteDrawingIfExists(sc, ln);
                continue;
            }

            const int idx = FindBarIndexForDateAndTime(sc, yyyymmdd, targetSec);
            if (idx < 0)
            {
                DeleteDrawingIfExists(sc, ln);
                continue;
            }

            const float price = GetAnchorPriceForBar(sc, idx, anchorMode);
            const SCDateTime beginDT = sc.BaseDateTimeIn[idx];

            DrawRay(sc, ln, beginDT, price, lineWidth, lineStyle, color);
        }
    }
}