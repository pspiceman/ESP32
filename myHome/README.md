# MyHome (GitHub Pages)

## Files
- `myhome.html`
- `icons/` (app icons + manifest)

## Deploy
Upload `myhome.html` and `icons/` to your repo, then open via GitHub Pages:
`https://<user>.github.io/<repo>/myhome.html`

## ESP32
ESP32 must be on the same Wi-Fi. This page auto-discovers ESP32 via:
1) saved base (localStorage)
2) http://myhome.local
3) scan 192.168.31.x, 192.168.0.x, 192.168.1.x (fast probe)

If it fails, press Refresh.


## Note (GitHub Pages path)
manifest.json is configured for /ESP32/myHome/.
If you move folders, update icons/manifest.json start_url + scope.

https://developer.tuya.com/en/
baekdc@naver.com
1700note//
