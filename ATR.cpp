#include "sierrachart.h"

SCDLLName("ATR + Range + Range/ATR (Numbers Rows)")

SCSFExport scsf_ATR_Range_Ratio_NumbersRows(SCStudyInterfaceRef sc)
{
    // Hidden working subgraphs
    SCSubgraphRef SG_ATR   = sc.Subgraph[0];
    SCSubgraphRef SG_Range = sc.Subgraph[1];
    SCSubgraphRef SG_Ratio = sc.Subgraph[2];

    // Inputs
    SCInputRef In_ATRLength        = sc.Input[0];
    SCInputRef In_Decimals         = sc.Input[1];
    SCInputRef In_RatioDecimals    = sc.Input[2];
    SCInputRef In_FontSize         = sc.Input[3];

    SCInputRef In_RowY_ATR         = sc.Input[4];
    SCInputRef In_RowY_Range       = sc.Input[5];
    SCInputRef In_RowY_Ratio       = sc.Input[6];

    SCInputRef In_FixedScale       = sc.Input[7];
    SCInputRef In_ScaleTop         = sc.Input[8];
    SCInputRef In_ScaleBottom      = sc.Input[9];
    SCInputRef In_DrawLastNBars    = sc.Input[10];

    if (sc.SetDefaults)
    {
        sc.GraphName        = "ATR + Range + Range/ATR (Numbers Rows)";
        sc.StudyDescription = "Displays 3 rows of per-bar numbers: ATR, Range (H-L), and Range/ATR.";
        sc.AutoLoop         = 1;

        // Put in its own region (change in Study Settings if desired)
        sc.GraphRegion = 1;

        // Hidden subgraphs
        SG_ATR.Name      = "ATR";
        SG_ATR.DrawStyle = DRAWSTYLE_HIDDEN;

        SG_Range.Name      = "Range";
        SG_Range.DrawStyle = DRAWSTYLE_HIDDEN;

        SG_Ratio.Name      = "Range/ATR";
        SG_Ratio.DrawStyle = DRAWSTYLE_HIDDEN;

        In_ATRLength.Name = "ATR Length";
        In_ATRLength.SetInt(14);
        In_ATRLength.SetIntLimits(1, 500);

        In_Decimals.Name = "Displayed Decimals (ATR & Range)";
        In_Decimals.SetInt(2);
        In_Decimals.SetIntLimits(0, 6);

        In_RatioDecimals.Name = "Displayed Decimals (Range/ATR)";
        In_RatioDecimals.SetInt(2);
        In_RatioDecimals.SetIntLimits(0, 6);

        In_FontSize.Name = "Font Size";
        In_FontSize.SetInt(8);
        In_FontSize.SetIntLimits(6, 24);

        // Three rows at different Y values within the study region scale
        In_RowY_ATR.Name = "Row Y Value: ATR";
        In_RowY_ATR.SetFloat(3.0f);

        In_RowY_Range.Name = "Row Y Value: Range";
        In_RowY_Range.SetFloat(2.0f);

        In_RowY_Ratio.Name = "Row Y Value: Range/ATR";
        In_RowY_Ratio.SetFloat(1.0f);

        In_FixedScale.Name = "Use Fixed Scale (recommended)";
        In_FixedScale.SetYesNo(1);

        In_ScaleTop.Name = "Fixed Scale Top";
        In_ScaleTop.SetFloat(4.0f);

        In_ScaleBottom.Name = "Fixed Scale Bottom";
        In_ScaleBottom.SetFloat(0.0f);

        In_DrawLastNBars.Name = "Draw Only Last N Bars (0 = all)";
        In_DrawLastNBars.SetInt(3000);
        In_DrawLastNBars.SetIntLimits(0, 200000);

        return;
    }

    const int atrLen = In_ATRLength.GetInt();
    sc.DataStartIndex = atrLen;

    // Stable panel scale helps keep rows visually consistent
    if (In_FixedScale.GetYesNo())
    {
        sc.ScaleRangeType   = SCALE_USERDEFINED;
        sc.ScaleRangeTop    = In_ScaleTop.GetFloat();
        sc.ScaleRangeBottom = In_ScaleBottom.GetFloat();
    }
    else
    {
        sc.ScaleRangeType = SCALE_AUTO;
    }

    // Compute ATR into hidden subgraph
    sc.ATR(sc.BaseDataIn, SG_ATR, atrLen, MOVAVGTYPE_SIMPLE);

    if (sc.Index < sc.DataStartIndex)
        return;

    // Optionally limit drawings to last N bars (performance)
    const int drawLastN = In_DrawLastNBars.GetInt();
    if (drawLastN > 0)
    {
        const int firstIndexToDraw = max(0, sc.ArraySize - drawLastN);
        if (sc.Index < firstIndexToDraw)
            return;
    }

    // Range and Ratio
    const float range = sc.High[sc.Index] - sc.Low[sc.Index];
    SG_Range[sc.Index] = range;

    const float atr = SG_ATR[sc.Index];
    SG_ATR[sc.Index] = atr;

    float ratio = 0.0f;
    if (atr > 0.0f)
        ratio = range / atr;

    SG_Ratio[sc.Index] = ratio;

    // Helper lambda to draw one cell of text
    auto DrawCell = [&](int rowId, float y, const SCString& text)
    {
        s_UseTool Tool;
        Tool.Clear();
        Tool.ChartNumber = sc.ChartNumber;
        Tool.DrawingType = DRAWING_TEXT;
        Tool.Region      = sc.GraphRegion;
        Tool.AddMethod   = UTAM_ADD_OR_ADJUST;

        // Unique line number per bar per row so each bar has its own “cell” for each row
        // rowId: 1=ATR, 2=Range, 3=Ratio
        Tool.LineNumber  = 1000000
            + sc.StudyGraphInstanceID * 500000
            + rowId * 200000
            + sc.Index;

        Tool.BeginIndex  = sc.Index;
        Tool.BeginValue  = y;

        Tool.Color       = RGB(255, 255, 255);
        Tool.FontSize    = In_FontSize.GetInt();
        Tool.FontBold    = 0;

        Tool.TextAlignment = DT_CENTER | DT_VCENTER;
        Tool.Text = text;

        sc.UseTool(Tool);
    };

    // Format and draw the 3 rows
    SCString txtATR, txtRange, txtRatio;

    txtATR.Format("%.*f", In_Decimals.GetInt(), atr);
    txtRange.Format("%.*f", In_Decimals.GetInt(), range);

    // Ratio: if ATR invalid/0, show blank (or you can show "NA")
    if (atr > 0.0f)
        txtRatio.Format("%.*f", In_RatioDecimals.GetInt(), ratio);
    else
        txtRatio = "";

    DrawCell(1, In_RowY_ATR.GetFloat(),   txtATR);
    DrawCell(2, In_RowY_Range.GetFloat(), txtRange);
    DrawCell(3, In_RowY_Ratio.GetFloat(), txtRatio);
}