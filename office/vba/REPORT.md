| Macro | Excel/Word gives | SG Office gives | Result |
|---|---|---|---|
| RangeAndCells | 50 | 50 | match |
| RangeFill | 21 | 21 | match |
| FormulaA1 | 6 | 6 | match |
| FormulaText | "=SUM(A1:A3)" | =SUM(A1:A3) | match |
| FormulaR1C1 | 10 | 10 | match |
| EndXlUp | 7 | 7 | match |
| ForEachCell | 15 | 15 | match |
| WorksheetFunctions | "30\|4\|2" | 30\|4\|2 | match |
| VLookupFunction | "Bob" | Bob | match |
| StringFunctions | "HELLO\|ell\|3\|a-b-c\|world\|2" | HELLO\|ell\|3\|a-b-c\|world\|2 | match |
| FormatNumber | "1,234.50\|45.60%\|0012" | 1,234.50\|45.60%\|0012 | match |
| DateFunctions | "2024-02-29\|2024\|3\|29" | 2024-02-29\|2024\|3\|29 | match |
| DateDiffDays | 60 | 60 | match |
| ArraysAndReDim | "3\|5\|9" | 3\|5\|9 | match |
| RangeToArray | "2\|3\|6" | 2\|3\|6 | match |
| ArrayToRange | 24 | 24 | match |
| CollectionObject | "3\|b" | 3\|b | match |
| ScriptingDictionary | "2\|20\|True" | #ERR 91: Object variable not set.
Additional information: An exception occurred 
Type: com.sun.star.lang.WrappedTargetRuntimeException
Message: [automation bridge] unexpected exception in IUnknownWrapper::getValue ! Message : 
. | error |
| OnErrorResumeNext | 11 | #ERR (never returned: LibreOffice hung or ended) | error |
| OnErrorGoTo | "caught 13" | caught 13 | match |
| SelectCaseAndLoops | "B\|10" | B\|10 | match |
| WithBlockFont | "True\|14" | True\|14 | match |
| InteriorColor | 255 | 255 | match |
| NumberFormat | "0.00%\|12.50%" | 0.00%\|12.50% | match |
| WorksheetsAddAndName | "Report\|Sheet1" | Report\|Report | differs |
| SheetsByName | 99 | 99 | match |
| CopyDestination | 3 | 3 | match |
| PasteSpecialValues | "6\|False" | 6\|False | match |
| SortRange | "Ann\|Cid" | Ann\|Cid | match |
| FindCell | 4 | 4 | match |
| OffsetResizeAddress | "$B$2:$C$3" | $B$2:$C$3 | match |
| CurrentRegionAndUsedRange | "3\|2" | 3\|2 | match |
| EvaluateAndBrackets | "3\|8" | #ERR 1: An exception occurred 
Type: com.sun.star.uno.RuntimeException
Message: . | error |
| NamedRange | 5 | 5 | match |
| InsertDeleteRows | "x\|3" | x\|3 | match |
| TypeConversions | "12\|3.5\|True\|False\|7" | 12\|3.5\|True\|False\|7 | match |
| FileIO | "line one" | line one | match |
| ApplicationProperties | "True\|Microsoft Excel" | True\|Microsoft Excel | match |
| UserDefinedFunctionInCell | 42 | 42 | match |
| AutoFilterVisibleRows | 2 | 3 | differs |
| CommentsAndHyperlinks | "note\|https://example.com/" | #ERR 1: An exception occurred 
Type: com.sun.star.uno.RuntimeException
Message: unsatisfied query for interface of type ooo.vba.excel.XWorksheet!. | error |
| ColumnWidthAndAlignment | "20\|-4108" | 20\|-4108 | match |
| ChartObjectsAdd | 1 | #ERR 1: An exception occurred 
Type: com.sun.star.uno.RuntimeException
Message: unsatisfied query for interface of type ooo.vba.excel.XWorksheet!. | error |
| ListObjectsTable | "Table1\|3" | #ERR 423: Property or method not found: ListObjects. | error |
| ContentText | "Hello world" | Hello world | match |
| InsertAfterParagraphs | 3 | #ERR 423: Property or method not found: InsertAfter. | error |
| SelectionTypeText | "typed" | typed | match |
| FindReplace | "a cat and a cat" | a cat and a cat | match |
| FontBold | -1 | -1 | match |
| TablesAdd | "x\|2" | #ERR 423: Property or method not found: Cell. | error |
| BookmarksAdd | "mark" | m | differs |
| ParagraphStyle | "Heading 1" | Heading 1 | match |
