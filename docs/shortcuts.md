# Keyboard shortcuts

```{index} keyboard shortcuts, shortcuts
```

On macOS, {kbd}`Command` stands where {kbd}`Ctrl` is written below.
Shortcuts that come from the platform — new, open, save, close, quit, back
and forward — follow its conventions, so on Windows *Save As* and *Quit*
have none.

## Running SQL

| Keys | What it does |
|---|---|
| {kbd}`Ctrl+Enter` | Run the statement under the cursor, or the selection |
| {kbd}`Ctrl+E` | Explain the statement |
| {kbd}`Ctrl+Shift+E` | Explain and run it, rolled back afterwards |
| {kbd}`Ctrl+.` | Stop what is running |

## Editing

| Keys | What it does |
|---|---|
| {kbd}`Ctrl+Space` | Ask for completions |
| {kbd}`Tab`, {kbd}`Enter` | Take the completion picked in the list |
| {kbd}`Esc` | Close the list of completions |
| {kbd}`Ctrl`+click | Open what a name is |
| {kbd}`Ctrl+/` | Comment the line, or the selected lines, out with `--`, or back in |
| {kbd}`Ctrl+F` | Find as you type |
| {kbd}`Ctrl+R` | Find and replace; in the results, run the query again |
| {kbd}`F3`, {kbd}`Shift+F3` | Find the next or the previous match; on macOS {kbd}`Command+G`, {kbd}`Shift+Command+G` |
| {kbd}`Ctrl+H`, {kbd}`Ctrl+Shift+H` | Find the selected text, or the word at the cursor, forwards or backwards |
| {kbd}`Enter`, {kbd}`Shift+Enter` | In the find field, the next or the previous match; in the replace field, replace |
| {kbd}`Esc` | Close the search bar; in the bar, so does the {kbd}`Ctrl+F` or {kbd}`Ctrl+R` that opened it |
| {kbd}`Ctrl+N` | Open a new SQL editor |
| {kbd}`Ctrl+O` | Open a file |
| {kbd}`Ctrl+S` | Save; in the result grid, save the edited rows |
| {kbd}`Ctrl+Shift+S` | Save as |
| {kbd}`Ctrl+W` | Close the page |
| {kbd}`Ctrl+Q` | Quit |

## Help

| Keys | What it does |
|---|---|
| {kbd}`F1` | Open the manual on the page for what you are looking at |
| {kbd}`Alt+Left`, {kbd}`Alt+Right` | Back and forward in the manual |

## Results and the tree

| Keys | What it does |
|---|---|
| {kbd}`F5`, {kbd}`Ctrl+R` | Run the query again, or reload the object's page or the tree; on macOS only {kbd}`Command+R`. In the editor {kbd}`Ctrl+R` is find and replace |
| {kbd}`Ctrl+Insert` | Add a row |
| {kbd}`Delete` | Delete the selected rows, or take back their deletion; in the tree, delete the connection |
| {kbd}`Ctrl+Shift+N` | Set the selected cells to NULL |
| {kbd}`Alt`+drag | Select a block in the text view |
