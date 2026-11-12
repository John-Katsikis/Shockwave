#!/usr/bin/env bash
# Builds the Effekseer editor 1.7.3.0 for Linux with Shockwave's patches
# (cmake/patches/effekseer), so the editor shows and saves what Shockwave plays, such as
# motion vector flipbooks. See docs/Effects.md, "The Effekseer editor".
#
#   cmake/BuildEffekseerEditor.sh [folder]
#
# folder defaults to ~/.local/share/shockwave/effekseer-editor. Running it again reuses the
# download, puts the patches on again and rebuilds what changed. When it's done, start the
# editor with <folder>/effekseer-editor [file.efkefc].
#
# Needs git, git-lfs, cmake, ninja, a C++ compiler, python3 and a .NET SDK (8 or newer;
# the editor targets .NET Core 3.1 and runs on the newer runtime). On Arch Linux:
#   sudo pacman -S --needed git git-lfs cmake ninja gcc python dotnet-sdk \
#       libx11 libxcb libxkbcommon-x11 mesa glu gtk3 libpulse openal zenity
set -euo pipefail

version=1.7.3.0
repo=https://github.com/effekseer/Effekseer.git
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
patch_dir=$here/patches/effekseer
dest=${1:-${XDG_DATA_HOME:-$HOME/.local/share}/shockwave/effekseer-editor}
src=$dest/Effekseer-$version
# Only the editor's own images and fonts are fetched from Git LFS, not the sample effects.
lfs_include="Dev/release/**,Dev/Cpp/Viewer/**,Resource/**"

missing=()
for tool in git git-lfs cmake ninja c++ python3 dotnet; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if ((${#missing[@]})); then
    echo "Missing: ${missing[*]}. See the top of $0 for what to install." >&2
    exit 1
fi

mkdir -p "$dest"

if [[ ! -d $src/.git ]]; then
    echo "== Downloading Effekseer $version"
    GIT_LFS_SKIP_SMUDGE=1 git clone --branch "$version" --depth 1 --recurse-submodules \
        --shallow-submodules "$repo" "$src"
fi
cd "$src"

echo "== Fetching the editor's images and fonts (Git LFS)"
git lfs install --local >/dev/null
if ! git lfs pull --include "$lfs_include"; then
    # GitHub sometimes refuses LFS downloads for busy repositories; the same files are
    # served as plain downloads too.
    echo "git lfs pull failed; downloading the files directly instead"
    git lfs ls-files -n --include "$lfs_include" | while read -r path; do
        if head -c 64 "$path" | grep -q '^version https://git-lfs'; then
            curl -fsSL --retry 3 -o "$path" \
                "https://media.githubusercontent.com/media/effekseer/Effekseer/$version/$path"
        fi
    done
fi

echo "== Applying Shockwave's patches"
# Put back every file a patch touches, then apply them all in name order, so a second run
# (or an edited patch) ends up the same as a first one.
patches=("$patch_dir"/*.patch)
mapfile -t touched < <(sed -n 's|^diff --git a/\(.*\) b/.*|\1|p' "${patches[@]}" | sort -u)
git checkout -- "${touched[@]}"
for patch in "${patches[@]}"; do
    git apply --whitespace=nowarn "$patch"
    echo "Applied $(basename "$patch")"
done

echo "== Building the viewer (C++)"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_VIEWER=ON \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.10
cmake --build build

echo "== Building the editor (C#)"
export DOTNET_CLI_TELEMETRY_OPTOUT=1
dotnet build Dev/Editor/Effekseer/Effekseer.Std.csproj -c Release

launcher=$dest/effekseer-editor
cat > "$launcher" <<EOF
#!/usr/bin/env bash
# Starts the Effekseer editor built by Shockwave's cmake/BuildEffekseerEditor.sh.
args=()
for arg in "\$@"; do
    if [[ -e \$arg ]]; then args+=("\$(realpath "\$arg")"); else args+=("\$arg"); fi
done
cd "$src/Dev/release"
# The editor targets .NET Core 3.1; let it run on the newer runtime that's installed.
export DOTNET_ROLL_FORWARD=Major
exec dotnet Effekseer.dll "\${args[@]}"
EOF
chmod +x "$launcher"

echo
echo "Done. Start the editor with:"
echo "  $launcher"
