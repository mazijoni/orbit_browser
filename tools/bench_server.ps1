<#
.SYNOPSIS
  Serves deterministic synthetic pages for scripts/benchmark.ps1 on
  http://localhost:<Port>/p/<n>. Each page is ~120 KB: an article with
  headings/paragraphs, a 200-row table, an inline SVG chart and a short
  script that builds part of the DOM — static after load (no timers), so idle
  measurements are not distorted.
#>
param([int] $Port = 8765)
$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()
Write-Host "bench server on http://localhost:$Port/"

function Get-Page([int] $n) {
  $sb = [System.Text.StringBuilder]::new()
  [void]$sb.Append("<!doctype html><html><head><meta charset=utf-8><title>Bench page $n</title><style>")
  [void]$sb.Append("body{font:15px/1.5 system-ui;margin:0;color:#222}header{background:#246;color:#fff;padding:16px 32px}")
  [void]$sb.Append("main{display:grid;grid-template-columns:2fr 1fr;gap:24px;padding:24px 32px}table{border-collapse:collapse;width:100%}")
  [void]$sb.Append("td,th{border:1px solid #ccc;padding:4px 8px}aside{background:#f4f4f6;padding:16px;border-radius:8px}</style></head><body>")
  [void]$sb.Append("<header><h1>Synthetic article $n</h1><nav>")
  for ($i = 0; $i -lt 12; $i++) { [void]$sb.Append("<a href='#s$i' style='color:#cde;margin-right:12px'>Section $i</a>") }
  [void]$sb.Append("</nav></header><main><article>")
  for ($s = 0; $s -lt 12; $s++) {
    [void]$sb.Append("<h2 id='s$s'>Section $s of page $n</h2>")
    for ($p = 0; $p -lt 6; $p++) {
      [void]$sb.Append("<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Paragraph $p.</p>")
    }
  }
  [void]$sb.Append("<table><thead><tr><th>#</th><th>Name</th><th>Value</th><th>Delta</th></tr></thead><tbody>")
  for ($r = 0; $r -lt 200; $r++) { [void]$sb.Append("<tr><td>$r</td><td>Item $r</td><td>$(($r * 37 + $n) % 1000)</td><td>$(($r * 13) % 97 - 48)</td></tr>") }
  [void]$sb.Append("</tbody></table></article><aside><svg width='300' height='200' viewBox='0 0 300 200'>")
  for ($b = 0; $b -lt 30; $b++) { $h = (($b * 53 + $n * 7) % 180) + 10; [void]$sb.Append("<rect x='$($b*10)' y='$(200-$h)' width='8' height='$h' fill='#48c'/>") }
  [void]$sb.Append("</svg><ul id='list'></ul></aside></main><script>")
  [void]$sb.Append("const ul=document.getElementById('list');for(let i=0;i<300;i++){const li=document.createElement('li');li.textContent='Generated entry '+i;ul.appendChild(li);}")
  [void]$sb.Append("</script></body></html>")
  return $sb.ToString()
}

try {
  while ($listener.IsListening) {
    $ctx = $listener.GetContext()
    $path = $ctx.Request.Url.AbsolutePath
    if ($path -match '^/p/(\d+)$') {
      $bytes = [Text.Encoding]::UTF8.GetBytes((Get-Page ([int]$Matches[1])))
      $ctx.Response.ContentType = 'text/html; charset=utf-8'
    } elseif ($path -eq '/quit') {
      $bytes = [Text.Encoding]::UTF8.GetBytes('bye'); $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
      $ctx.Response.Close(); break
    } else {
      $ctx.Response.StatusCode = 404; $bytes = [Text.Encoding]::UTF8.GetBytes('not found')
    }
    $ctx.Response.ContentLength64 = $bytes.Length
    $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
    $ctx.Response.Close()
  }
} finally { $listener.Stop() }
