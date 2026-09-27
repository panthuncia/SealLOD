# Set the path to the directory you want to search
$directory = "."

# Set a list of directory names to exclude
$excludedDirectories = @(
    "nlohmann", "stb", "DirectX", "pix", "Intel", "FidelityFX", "ThirdParty", 
    ".*", "models", "textures", "out", "Aftermath", "NVSL", "vcpkg_installed", 
    "tree-sitter-hlsl", "volk", "GPU-Reshape", "openpbr-bsdf", "ImGuiColorTextEdit",
    "xatlas", "PyNifly", "geometry-central", "FidelityFX-SDK")

# File extensions to include in the search (e.g., .txt, .cpp, .h)
$includeExtensions = @("*.txt", "*.cpp", "*.h")

# Use Get-ChildItem to get all matching files in the directory recursively
$files = Get-ChildItem -Path $directory -Recurse -Include $includeExtensions |
    Where-Object { 
        # Check if any part of the directory path contains an excluded directory name
        $exclude = $false
        foreach ($excluded in $excludedDirectories) {
            if ($_.FullName -like "*\$excluded\*") {
                $exclude = $true
                break
            }
        }
        return -not $exclude
    }

# Initialize a counter for the total number of lines
$totalLines = 0

# Loop through each file, print the filename, and count the lines
foreach ($file in $files) {
    Write-Host "Processing file: $($file.FullName)"
    $lineCount = (Get-Content $file.FullName).Count
    $totalLines += $lineCount
}

# Output the total number of lines
Write-Host "Total number of lines: $totalLines"