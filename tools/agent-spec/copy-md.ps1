# copy agent-spec.md to the user's desktop

$dest = Join-Path ([Environment]::GetFolderPath('Desktop')) 'TeacherBox-工具规范.md'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'agent-spec.md') -Destination $dest -Force
Write-Output $dest
