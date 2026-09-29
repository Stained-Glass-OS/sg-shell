' SG Office -- the Excel VBA corpus: macros as people write them in Excel,
' each a Function whose result is what Excel's VBA gives. Every test runs in
' its own module (a compile error in one cannot hide the others), on a
' worksheet the runner clears first. Format:
'     '@test NAME => expected      (expected as in corpus-1.txt: 42, "text", TRUE)
'     Function NAME() ... End Function
' The runner (office/vba/vba.py) wraps each in an error handler, so a
' runtime error comes back as "#ERR <number>: <description>".
'
' Copyright (C) 2026 Stained Glass OS contributors
' SPDX-License-Identifier: AGPL-3.0-or-later

'@test RangeAndCells => 50
Function RangeAndCells()
    Range("A1").Value = 42
    Cells(2, 1).Value = 8
    RangeAndCells = Range("A1").Value + Cells(2, 1).Value
End Function

'@test RangeFill => 21
Function RangeFill()
    Range("B1:B3").Value = 7
    RangeFill = Application.WorksheetFunction.Sum(Range("B1:B3"))
End Function

'@test FormulaA1 => 6
Function FormulaA1()
    Range("A1").Value = 1: Range("A2").Value = 2: Range("A3").Value = 3
    Range("A4").Formula = "=SUM(A1:A3)"
    FormulaA1 = Range("A4").Value
End Function

'@test FormulaText => "=SUM(A1:A3)"
Function FormulaText()
    Range("A4").Formula = "=SUM(A1:A3)"
    FormulaText = Range("A4").Formula
End Function

'@test FormulaR1C1 => 10
Function FormulaR1C1()
    Range("A1").Value = 4: Range("A2").Value = 6
    Range("B2").FormulaR1C1 = "=R[-1]C[-1]+RC[-1]"
    FormulaR1C1 = Range("B2").Value
End Function

'@test EndXlUp => 7
Function EndXlUp()
    Dim i As Long
    For i = 1 To 7
        Cells(i, 1).Value = i
    Next i
    EndXlUp = Cells(Rows.Count, 1).End(xlUp).Row
End Function

'@test ForEachCell => 15
Function ForEachCell()
    Dim c As Range, total As Double, i As Integer
    For i = 1 To 5: Cells(i, 3).Value = i: Next i
    For Each c In Range("C1:C5")
        total = total + c.Value
    Next c
    ForEachCell = total
End Function

'@test WorksheetFunctions => "30|4|2"
Function WorksheetFunctions()
    Range("A1:A4").Value = Application.Transpose(Array(5, 10, 15, 0))
    Range("B1").Value = "x": Range("B2").Value = "y"
    WorksheetFunctions = Application.WorksheetFunction.Sum(Range("A1:A4")) & "|" & _
        Application.WorksheetFunction.Count(Range("A1:A4")) & "|" & _
        Application.WorksheetFunction.CountA(Range("B1:B9"))
End Function

'@test VLookupFunction => "Bob"
Function VLookupFunction()
    Range("A1").Value = 1: Range("B1").Value = "Ann"
    Range("A2").Value = 2: Range("B2").Value = "Bob"
    VLookupFunction = Application.WorksheetFunction.VLookup(2, Range("A1:B2"), 2, False)
End Function

'@test StringFunctions => "HELLO|ell|3|a-b-c|world|2"
Function StringFunctions()
    Dim parts
    parts = Split("a b c", " ")
    StringFunctions = UCase("hello") & "|" & Mid("hello", 2, 3) & "|" & InStr("hello", "l") & "|" & _
        Join(parts, "-") & "|" & Trim("  world  ") & "|" & Len(Replace("abab", "a", ""))
End Function

'@test FormatNumber => "1,234.50|45.60%|0012"
Function FormatNumber()
    FormatNumber = Format(1234.5, "#,##0.00") & "|" & Format(0.456, "0.00%") & "|" & Format(12, "0000")
End Function

'@test DateFunctions => "2024-02-29|2024|3|29"
Function DateFunctions()
    Dim d As Date
    d = DateSerial(2024, 1, 31)
    d = DateAdd("m", 1, d)
    DateFunctions = Format(d, "yyyy-mm-dd") & "|" & Year(d) & "|" & Month(DateAdd("d", 1, d)) & "|" & Day(d)
End Function

'@test DateDiffDays => 60
Function DateDiffDays()
    DateDiffDays = DateDiff("d", DateSerial(2024, 1, 1), DateSerial(2024, 3, 1))
End Function

'@test ArraysAndReDim => "3|5|9"
Function ArraysAndReDim()
    Dim a() As Integer
    ReDim a(1 To 3)
    a(1) = 2: a(2) = 3: a(3) = 4
    ReDim Preserve a(1 To 5)
    a(5) = 9
    ArraysAndReDim = (a(1) + a(3) - a(2)) & "|" & UBound(a) & "|" & a(5)
End Function

'@test RangeToArray => "2|3|6"
Function RangeToArray()
    Dim v
    Range("A1").Value = 1: Range("B1").Value = 2: Range("C1").Value = 3
    Range("A2").Value = 4: Range("B2").Value = 5: Range("C2").Value = 6
    v = Range("A1:C2").Value
    RangeToArray = UBound(v, 1) & "|" & UBound(v, 2) & "|" & v(2, 3)
End Function

'@test ArrayToRange => 24
Function ArrayToRange()
    Dim v(1 To 2, 1 To 2)
    v(1, 1) = 3: v(1, 2) = 5: v(2, 1) = 7: v(2, 2) = 9
    Range("D1:E2").Value = v
    ArrayToRange = Application.WorksheetFunction.Sum(Range("D1:E2"))
End Function

'@test CollectionObject => "3|b"
Function CollectionObject()
    Dim c As New Collection
    c.Add "a": c.Add "b": c.Add "c"
    CollectionObject = c.Count & "|" & c(2)
End Function

'@test ScriptingDictionary => "2|20|True"
Function ScriptingDictionary()
    Dim d As Object
    Set d = CreateObject("Scripting.Dictionary")
    d.Add "a", 10
    d("b") = 20
    ScriptingDictionary = d.Count & "|" & d("b") & "|" & d.Exists("a")
End Function

'@test OnErrorResumeNext => 11
Function OnErrorResumeNext()
    Dim x
    On Error Resume Next
    x = 1 / 0
    OnErrorResumeNext = Err.Number
End Function

'@test OnErrorGoTo => "caught 13"
Function OnErrorGoTo()
    Dim n As Integer
    On Error GoTo bad
    n = CInt("abc")
    OnErrorGoTo = "no error"
    Exit Function
bad:
    OnErrorGoTo = "caught " & Err.Number
End Function

'@test SelectCaseAndLoops => "B|10"
Function SelectCaseAndLoops()
    Dim g As String, n As Integer, i As Integer
    Select Case 85
        Case Is >= 90: g = "A"
        Case 80 To 89: g = "B"
        Case Else: g = "C"
    End Select
    Do While i < 4
        i = i + 1
        n = n + i
    Loop
    SelectCaseAndLoops = g & "|" & n
End Function

'@test WithBlockFont => "True|14"
Function WithBlockFont()
    With Range("A1").Font
        .Bold = True
        .Size = 14
    End With
    WithBlockFont = Range("A1").Font.Bold & "|" & Range("A1").Font.Size
End Function

'@test InteriorColor => 255
Function InteriorColor()
    Range("A1").Interior.Color = RGB(255, 0, 0)
    InteriorColor = Range("A1").Interior.Color
End Function

'@test NumberFormat => "0.00%|12.50%"
Function NumberFormat()
    Range("A1").Value = 0.125
    Range("A1").NumberFormat = "0.00%"
    NumberFormat = Range("A1").NumberFormat & "|" & Range("A1").Text
End Function

'@test WorksheetsAddAndName => "Report|Sheet1"
Function WorksheetsAddAndName()
    Dim ws As Worksheet
    Set ws = Worksheets.Add(After:=Worksheets(Worksheets.Count))
    ws.Name = "Report"
    WorksheetsAddAndName = Worksheets(Worksheets.Count).Name & "|" & Worksheets(1).Name
    Application.DisplayAlerts = False
    ws.Delete
    Application.DisplayAlerts = True
End Function

'@test SheetsByName => 99
Function SheetsByName()
    Sheets("Sheet1").Range("F1").Value = 99
    SheetsByName = Worksheets("Sheet1").Range("F1").Value
End Function

'@test CopyDestination => 3
Function CopyDestination()
    Range("A1:A3").Value = 1
    Range("A1:A3").Copy Destination:=Range("D1")
    CopyDestination = Application.WorksheetFunction.Sum(Range("D1:D3"))
End Function

'@test PasteSpecialValues => "6|False"
Function PasteSpecialValues()
    Range("A1").Value = 1: Range("A2").Value = 5
    Range("A3").Formula = "=A1+A2"
    Range("A3").Copy
    Range("C3").PasteSpecial Paste:=xlPasteValues
    Application.CutCopyMode = False
    PasteSpecialValues = Range("C3").Value & "|" & Range("C3").HasFormula
End Function

'@test SortRange => "Ann|Cid"
Function SortRange()
    Range("A1").Value = "Name"
    Range("A2").Value = "Cid": Range("A3").Value = "Ann": Range("A4").Value = "Bob"
    Range("A1:A4").Sort Key1:=Range("A1"), Order1:=xlAscending, Header:=xlYes
    SortRange = Range("A2").Value & "|" & Range("A4").Value
End Function

'@test FindCell => 4
Function FindCell()
    Range("A1:A5").Value = Application.Transpose(Array("a", "b", "c", "needle", "e"))
    Dim f As Range
    Set f = Range("A1:A5").Find("needle")
    FindCell = f.Row
End Function

'@test OffsetResizeAddress => "$B$2:$C$3"
Function OffsetResizeAddress()
    OffsetResizeAddress = Range("A1").Offset(1, 1).Resize(2, 2).Address
End Function

'@test CurrentRegionAndUsedRange => "3|2"
Function CurrentRegionAndUsedRange()
    Range("A1:B3").Value = 1
    CurrentRegionAndUsedRange = Range("A1").CurrentRegion.Rows.Count & "|" & ActiveSheet.UsedRange.Columns.Count
End Function

'@test EvaluateAndBrackets => "3|8"
Function EvaluateAndBrackets()
    Range("A1").Value = 8
    EvaluateAndBrackets = Application.Evaluate("SUM(1,2)") & "|" & [A1]
End Function

'@test NamedRange => 5
Function NamedRange()
    Range("A1").Value = 5
    ThisWorkbook.Names.Add Name:="Total", RefersTo:="=Sheet1!$A$1"
    NamedRange = Range("Total").Value
End Function

'@test InsertDeleteRows => "x|3"
Function InsertDeleteRows()
    Range("A1").Value = "x": Range("A2").Value = "y": Range("A3").Value = "z"
    Rows(2).Delete
    Rows(1).Insert
    InsertDeleteRows = Range("A2").Value & "|" & Cells(Rows.Count, 1).End(xlUp).Row
End Function

'@test TypeConversions => "12|3.5|True|False|7"
Function TypeConversions()
    TypeConversions = CInt("12") & "|" & CDbl("3.5") & "|" & IsNumeric("42") & "|" & IsEmpty(1) & "|" & Val("7 apples")
End Function

'@test FileIO => "line one"
Function FileIO()
    Dim p As String, s As String
    p = Environ("TEMP") & "\sg-vba-test.txt"
    Open p For Output As #1
    Print #1, "line one"
    Close #1
    Open p For Input As #1
    Line Input #1, s
    Close #1
    Kill p
    FileIO = s
End Function

'@test ApplicationProperties => "True|Microsoft Excel"
Function ApplicationProperties()
    Application.ScreenUpdating = False
    Application.ScreenUpdating = True
    ApplicationProperties = (Application.Calculation = xlCalculationAutomatic) & "|" & Application.Name
End Function

'@test UserDefinedFunctionInCell => 42
Function UserDefinedFunctionInCell()
    Range("A1").Formula = "=SGDouble(21)"
    Application.Calculate
    UserDefinedFunctionInCell = Range("A1").Value
End Function
Function SGDouble(x)
    SGDouble = x * 2
End Function

'@test AutoFilterVisibleRows => 2
Function AutoFilterVisibleRows()
    Range("A1").Value = "n"
    Range("A2:A6").Value = Application.Transpose(Array(1, 2, 3, 4, 5))
    Range("A1:A6").AutoFilter Field:=1, Criteria1:=">3"
    AutoFilterVisibleRows = Range("A2:A6").SpecialCells(xlCellTypeVisible).Count
    ActiveSheet.AutoFilterMode = False
End Function

'@test CommentsAndHyperlinks => "note|https://example.com/"
Function CommentsAndHyperlinks()
    Range("A1").AddComment "note"
    ActiveSheet.Hyperlinks.Add Anchor:=Range("B1"), Address:="https://example.com/", TextToDisplay:="site"
    CommentsAndHyperlinks = Range("A1").Comment.Text & "|" & Range("B1").Hyperlinks(1).Address
End Function

'@test ColumnWidthAndAlignment => "20|-4108"
Function ColumnWidthAndAlignment()
    Columns("A").ColumnWidth = 20
    Range("A1").HorizontalAlignment = xlCenter
    ColumnWidthAndAlignment = Columns("A").ColumnWidth & "|" & Range("A1").HorizontalAlignment
End Function

'@test ChartObjectsAdd => 1
Function ChartObjectsAdd()
    Range("A1:A3").Value = Application.Transpose(Array(1, 2, 3))
    Dim co As ChartObject
    Set co = ActiveSheet.ChartObjects.Add(Left:=100, Top:=10, Width:=300, Height:=200)
    co.Chart.SetSourceData Source:=Range("A1:A3")
    ChartObjectsAdd = ActiveSheet.ChartObjects.Count
    co.Delete
End Function

'@test ListObjectsTable => "Table1|3"
Function ListObjectsTable()
    Range("A1").Value = "Name": Range("B1").Value = "Units"
    Range("A2").Value = "Ann": Range("B2").Value = 10
    Range("A3").Value = "Bob": Range("B3").Value = 20
    Dim lo As ListObject
    Set lo = ActiveSheet.ListObjects.Add(xlSrcRange, Range("A1:B3"), , xlYes)
    lo.Name = "Table1"
    ListObjectsTable = lo.Name & "|" & lo.Range.Rows.Count
End Function
