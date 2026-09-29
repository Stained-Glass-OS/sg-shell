' SG Office -- the Word VBA corpus: document macros as people write them in
' Word; same format as excel.vba. Each runs on an emptied document.
'
' Copyright (C) 2026 Stained Glass OS contributors
' SPDX-License-Identifier: AGPL-3.0-or-later

'@test ContentText => "Hello world"
Function ContentText()
    ActiveDocument.Content.Text = "Hello world"
    ContentText = Replace(ActiveDocument.Content.Text, vbCr, "")
End Function

'@test InsertAfterParagraphs => 3
Function InsertAfterParagraphs()
    ActiveDocument.Content.Text = "One"
    ActiveDocument.Content.InsertAfter vbCr & "Two"
    ActiveDocument.Content.InsertAfter vbCr & "Three"
    InsertAfterParagraphs = ActiveDocument.Paragraphs.Count
End Function

'@test SelectionTypeText => "typed"
Function SelectionTypeText()
    Selection.TypeText "typed"
    SelectionTypeText = Replace(ActiveDocument.Content.Text, vbCr, "")
End Function

'@test FindReplace => "a cat and a cat"
Function FindReplace()
    ActiveDocument.Content.Text = "a dog and a dog"
    With ActiveDocument.Content.Find
        .Text = "dog"
        .Replacement.Text = "cat"
        .Execute Replace:=wdReplaceAll
    End With
    FindReplace = Replace(ActiveDocument.Content.Text, vbCr, "")
End Function

' Word's Font.Bold is a Long: True is -1 (wdToggle and wdUndefined are others)
'@test FontBold => -1
Function FontBold()
    ActiveDocument.Content.Text = "bold me"
    ActiveDocument.Content.Font.Bold = True
    FontBold = ActiveDocument.Content.Font.Bold
End Function

'@test TablesAdd => "x|2"
Function TablesAdd()
    Dim t As Table
    Set t = ActiveDocument.Tables.Add(ActiveDocument.Content, 2, 3)
    t.Cell(1, 1).Range.Text = "x"
    TablesAdd = Left(t.Cell(1, 1).Range.Text, 1) & "|" & t.Rows.Count
End Function

'@test BookmarksAdd => "mark"
Function BookmarksAdd()
    ActiveDocument.Content.Text = "mark here"
    Dim r As Range
    Set r = ActiveDocument.Range(0, 4)
    ActiveDocument.Bookmarks.Add Name:="B1", Range:=r
    BookmarksAdd = ActiveDocument.Bookmarks("B1").Range.Text
End Function

'@test ParagraphStyle => "Heading 1"
Function ParagraphStyle()
    ActiveDocument.Content.Text = "Title"
    ActiveDocument.Paragraphs(1).Style = ActiveDocument.Styles("Heading 1")
    ParagraphStyle = ActiveDocument.Paragraphs(1).Style
End Function
