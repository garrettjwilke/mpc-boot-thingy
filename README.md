## mpc boot thingy

this thingy will allow various MPC software like custom firmware and custom applications to work without breaking eachother.
you can install applications and choose between them easily from the menu.

there are other enhancements this provides, but ehh...they're there...there :)

[![mpc sample boot thingy video](https://youtu.be/eMhiaCJpgGg)](https://youtu.be/eMhiaCJpgGg)

## web page

Edit the page in `webserver/source`. `./setup.sh` builds that into `webserver/content` and copies the directory onto the card. The generated content is not committed.

```
cd webserver/source
npm install
npm run dev
```

`npm run dev` serves the page with sample boot entries so the layout can be checked without an MPC. `npm run dev:live` uses a server from `make web-host` on port 8080 instead.
