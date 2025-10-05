# Usage with waybar
```jsonc
"image#mpris-cover": {
  "path": "/home/$user/.cache/mpris-cover.png",
  "signal": 5
},
"custom/mpris": {
  "exec": "$path_to_exe_from_this_project",
  "return-type": "json",
  "format": "{text}"
},
```
