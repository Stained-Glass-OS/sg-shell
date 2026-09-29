"""SG Office Functions -- what the add-in offers: the Excel functions
LibreOffice's engine lacks, each under Excel's own name, so a workbook from
Excel that uses them calculates, and saving it writes them back as Excel
spells them (the "compatibility name", "_xlfn." included).

One entry a function:
    (method, name, excel name in files, category, description,
     [(parameter, description, kind)], returns)
kind: "any" (optional unless listed in REQUIRED), "rest" (any number more);
returns: "any" (one value) or "array".

build-oxt.py makes the type library (XSgFunctions.idl) and the Calc add-in
configuration (CalcAddIn.xcu) from this table.

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""

IMPL = "org.stainedglass.office.Functions"
VERSION = "1.0"
INTERFACE = "org.stainedglass.office.XSgFunctions"

A = "any"
R = "rest"

FUNCTIONS = [
    # ---- text ----
    ("textbefore", "TEXTBEFORE", "_xlfn.TEXTBEFORE", "Text", "Returns the text before a delimiter.",
     [("text", "The text to search.", A), ("delimiter", "What marks the point to extract before.", A),
      ("instance_num", "Which occurrence of the delimiter (negative: from the end).", A),
      ("match_mode", "1 ignores case.", A), ("match_end", "1 treats the end of the text as a delimiter.", A),
      ("if_not_found", "Returned when there is no match.", A)], "any"),
    ("textafter", "TEXTAFTER", "_xlfn.TEXTAFTER", "Text", "Returns the text after a delimiter.",
     [("text", "The text to search.", A), ("delimiter", "What marks the point to extract after.", A),
      ("instance_num", "Which occurrence of the delimiter (negative: from the end).", A),
      ("match_mode", "1 ignores case.", A), ("match_end", "1 treats the end of the text as a delimiter.", A),
      ("if_not_found", "Returned when there is no match.", A)], "any"),
    ("textsplit", "TEXTSPLIT", "_xlfn.TEXTSPLIT", "Text", "Splits text into columns and rows by delimiters.",
     [("text", "The text to split.", A), ("col_delimiter", "Where to split into columns.", A),
      ("row_delimiter", "Where to split into rows.", A), ("ignore_empty", "TRUE skips empty values.", A),
      ("match_mode", "1 ignores case.", A), ("pad_with", "What fills missing values (default #N/A).", A)], "array"),
    ("arraytotext", "ARRAYTOTEXT", "_xlfn.ARRAYTOTEXT", "Text", "Returns an array's values as text.",
     [("array", "The array.", A), ("format", "0 concise, 1 strict.", A)], "any"),
    ("valuetotext", "VALUETOTEXT", "_xlfn.VALUETOTEXT", "Text", "Returns a value as text.",
     [("value", "The value.", A), ("format", "0 concise, 1 strict (text in quotes).", A)], "any"),
    ("regextest", "REGEXTEST", "_xlfn.REGEXTEST", "Text", "Whether text matches a regular expression.",
     [("text", "The text.", A), ("pattern", "The regular expression.", A),
      ("case_sensitivity", "0 case-sensitive, 1 not.", A)], "any"),
    ("regexextract", "REGEXEXTRACT", "_xlfn.REGEXEXTRACT", "Text", "Extracts text matching a regular expression.",
     [("text", "The text.", A), ("pattern", "The regular expression.", A),
      ("return_mode", "0 first match, 1 all matches, 2 the first match's groups.", A),
      ("case_sensitivity", "0 case-sensitive, 1 not.", A)], "array"),
    ("regexreplace", "REGEXREPLACE", "_xlfn.REGEXREPLACE", "Text", "Replaces text matching a regular expression.",
     [("text", "The text.", A), ("pattern", "The regular expression.", A), ("replacement", "The replacement ($1 for a group).", A),
      ("occurrence", "0 all, n the nth, -n the nth from the end.", A), ("case_sensitivity", "0 case-sensitive, 1 not.", A)], "any"),
    ("dbcs", "DBCS", "DBCS", "Text", "Changes half-width characters to full-width ones.",
     [("text", "The text.", A)], "any"),
    ("phonetic", "PHONETIC", "PHONETIC", "Text", "The phonetic (furigana) text; the text itself when it has none.",
     [("reference", "The cells.", A)], "any"),
    # ---- arrays ----
    ("vstack", "VSTACK", "_xlfn.VSTACK", "Spreadsheet", "Stacks arrays vertically.",
     [("array1", "The first array.", A), ("arrays", "More arrays.", R)], "array"),
    ("hstack", "HSTACK", "_xlfn.HSTACK", "Spreadsheet", "Stacks arrays horizontally.",
     [("array1", "The first array.", A), ("arrays", "More arrays.", R)], "array"),
    ("torow", "TOROW", "_xlfn.TOROW", "Spreadsheet", "Returns an array as one row.",
     [("array", "The array.", A), ("ignore", "1 blanks, 2 errors, 3 both.", A), ("scan_by_column", "TRUE reads by column.", A)], "array"),
    ("tocol", "TOCOL", "_xlfn.TOCOL", "Spreadsheet", "Returns an array as one column.",
     [("array", "The array.", A), ("ignore", "1 blanks, 2 errors, 3 both.", A), ("scan_by_column", "TRUE reads by column.", A)], "array"),
    ("wraprows", "WRAPROWS", "_xlfn.WRAPROWS", "Spreadsheet", "Wraps a row or column into rows.",
     [("vector", "The values.", A), ("wrap_count", "Values a row.", A), ("pad_with", "What fills the last row.", A)], "array"),
    ("wrapcols", "WRAPCOLS", "_xlfn.WRAPCOLS", "Spreadsheet", "Wraps a row or column into columns.",
     [("vector", "The values.", A), ("wrap_count", "Values a column.", A), ("pad_with", "What fills the last column.", A)], "array"),
    ("take", "TAKE", "_xlfn.TAKE", "Spreadsheet", "Rows or columns from the start or end of an array.",
     [("array", "The array.", A), ("rows", "How many rows (negative: from the end).", A),
      ("columns", "How many columns (negative: from the end).", A)], "array"),
    ("drop", "DROP", "_xlfn.DROP", "Spreadsheet", "An array without rows or columns from its start or end.",
     [("array", "The array.", A), ("rows", "How many rows to drop (negative: from the end).", A),
      ("columns", "How many columns to drop (negative: from the end).", A)], "array"),
    ("chooserows", "CHOOSEROWS", "_xlfn.CHOOSEROWS", "Spreadsheet", "The given rows of an array.",
     [("array", "The array.", A), ("row_num1", "A row number (negative: from the end).", A), ("row_nums", "More row numbers.", R)], "array"),
    ("choosecols", "CHOOSECOLS", "_xlfn.CHOOSECOLS", "Spreadsheet", "The given columns of an array.",
     [("array", "The array.", A), ("col_num1", "A column number (negative: from the end).", A), ("col_nums", "More column numbers.", R)], "array"),
    ("expand", "EXPAND", "_xlfn.EXPAND", "Spreadsheet", "Expands an array to the given size.",
     [("array", "The array.", A), ("rows", "Rows.", A), ("columns", "Columns.", A), ("pad_with", "What fills the new cells.", A)], "array"),
    ("trimrange", "TRIMRANGE", "_xlfn.TRIMRANGE", "Spreadsheet", "A range's values without its empty outer rows and columns.",
     [("range", "The range.", A), ("trim_rows", "0 none, 1 leading, 2 trailing, 3 both.", A),
      ("trim_cols", "0 none, 1 leading, 2 trailing, 3 both.", A)], "array"),
    # ---- math, statistics, engineering ----
    ("percentof", "PERCENTOF", "_xlfn.PERCENTOF", "Mathematical", "The share a subset is of the whole.",
     [("data_subset", "The part.", A), ("data_all", "The whole.", A)], "any"),
    ("binomdistrange", "BINOM.DIST.RANGE", "_xlfn.BINOM.DIST.RANGE", "Statistical",
     "The probability of a number of successes, or a range of them, in a binomial distribution.",
     [("trials", "Independent trials.", A), ("probability_s", "The probability of success in each.", A),
      ("number_s", "Successes.", A), ("number_s2", "Up to this many successes.", A)], "any"),
] + [
    ("im" + f, "IM" + f.upper(), "_xlfn.IM" + f.upper(), "Add-In", desc,
     [("inumber", "A complex number (x+yi).", A)], "any")
    for f, desc in (("cosh", "The hyperbolic cosine of a complex number."),
                    ("cot", "The cotangent of a complex number."),
                    ("csc", "The cosecant of a complex number."),
                    ("csch", "The hyperbolic cosecant of a complex number."),
                    ("sec", "The secant of a complex number."),
                    ("sech", "The hyperbolic secant of a complex number."),
                    ("sinh", "The hyperbolic sine of a complex number."),
                    ("tan", "The tangent of a complex number."))
]

# Parameters that must be given (the rest may be left out, as in Excel).
REQUIRED = {
    "textbefore": 2, "textafter": 2, "textsplit": 1, "arraytotext": 1, "valuetotext": 1, "regextest": 2,
    "regexextract": 2, "regexreplace": 3, "dbcs": 1, "phonetic": 1, "vstack": 1, "hstack": 1, "torow": 1,
    "tocol": 1, "wraprows": 2, "wrapcols": 2, "take": 1, "drop": 1, "chooserows": 2, "choosecols": 2,
    "expand": 2, "trimrange": 1, "percentof": 2, "binomdistrange": 3,
}
