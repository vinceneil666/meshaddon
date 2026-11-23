<?php
// MeshAddon: read-only proxy to the official MeshCore flasher for Apache/PHP hosting
// (replaces Caddy's /mc/ reverse proxy; flasher.meshcore.io sends no CORS headers).
// .htaccess rewrites  mc/<path>  ->  mc.php?p=<path>
// Only the catalogue, the release list and release binaries are allowed.
$p = isset($_GET['p']) ? $_GET['p'] : '';
if ($p === 'config.json') {
    $url = '/config.json'; $ttl = 600;
} elseif ($p === 'releases' && isset($_GET['repo']) && preg_match('/^[a-z0-9_-]{1,32}$/', $_GET['repo'])) {
    $url = '/releases?repo=' . $_GET['repo']; $ttl = 600;
} elseif (preg_match('#^releases/download/[A-Za-z0-9._-]{1,80}/[A-Za-z0-9._-]{1,160}\.bin$#', $p) && strpos($p, '..') === false) {
    $url = '/' . $p; $ttl = 0;
} else {
    http_response_code(404); header('Content-Type: text/plain'); exit("not allowed\n");
}

// small file cache for the JSON (polite to the upstream, fast for users)
$cdir = __DIR__ . '/.cache';
$cfile = $cdir . '/' . sha1($url) . '.json';
if ($ttl && is_file($cfile) && time() - filemtime($cfile) < $ttl) {
    header('Content-Type: application/json'); header('Cache-Control: max-age=300');
    readfile($cfile); exit;
}

$ch = curl_init('https://flasher.meshcore.io' . $url);
curl_setopt_array($ch, [
    CURLOPT_RETURNTRANSFER => true, CURLOPT_FOLLOWLOCATION => true, CURLOPT_MAXREDIRS => 5,
    CURLOPT_PROTOCOLS => CURLPROTO_HTTPS, CURLOPT_REDIR_PROTOCOLS => CURLPROTO_HTTPS,
    CURLOPT_CONNECTTIMEOUT => 10, CURLOPT_TIMEOUT => 60, CURLOPT_USERAGENT => 'meshaddon-configurator/0.1',
]);
$body = curl_exec($ch);
$code = curl_getinfo($ch, CURLINFO_RESPONSE_CODE);
curl_close($ch);
if ($body === false || $code !== 200) {
    if ($ttl && is_file($cfile)) { header('Content-Type: application/json'); readfile($cfile); exit; }   // stale is better than nothing
    http_response_code(502); header('Content-Type: text/plain'); exit("upstream error ($code)\n");
}
if ($ttl) {
    if (!json_decode($body)) { http_response_code(502); exit("bad upstream json\n"); }
    @mkdir($cdir, 0755); @file_put_contents($cfile . '.tmp', $body) && @rename($cfile . '.tmp', $cfile);
    header('Content-Type: application/json'); header('Cache-Control: max-age=300');
} else {
    header('Content-Type: application/octet-stream'); header('Cache-Control: max-age=86400');
}
header('Content-Length: ' . strlen($body));
echo $body;
