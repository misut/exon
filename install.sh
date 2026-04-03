#!/bin/sh
set -e

REPO="misut/exon"
INSTALL_DIR="$HOME/.exon/bin"

# Platform detection
OS=$(uname -s)
ARCH=$(uname -m)

if [ "$OS" != "Darwin" ] || [ "$ARCH" != "arm64" ]; then
    echo "error: only macOS ARM (Apple Silicon) is currently supported"
    echo "  detected: $OS $ARCH"
    exit 1
fi

PLATFORM="aarch64-apple-darwin"

# Get latest release tag
echo "fetching latest release..."
TAG=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | grep '"tag_name"' | sed 's/.*"tag_name": "\(.*\)".*/\1/')

if [ -z "$TAG" ]; then
    echo "error: failed to fetch latest release"
    exit 1
fi

echo "installing exon $TAG..."

# Download
URL="https://github.com/$REPO/releases/download/$TAG/exon-$TAG-$PLATFORM.tar.gz"
TMPDIR=$(mktemp -d)
curl -fsSL "$URL" -o "$TMPDIR/exon.tar.gz"

# Extract and install
tar -xzf "$TMPDIR/exon.tar.gz" -C "$TMPDIR"
mkdir -p "$INSTALL_DIR"
mv "$TMPDIR/exon" "$INSTALL_DIR/exon"
chmod +x "$INSTALL_DIR/exon"
rm -rf "$TMPDIR"

echo "installed exon $TAG to $INSTALL_DIR/exon"
echo ""

# PATH check
case ":$PATH:" in
    *":$INSTALL_DIR:"*) ;;
    *)
        echo "add this to your shell profile:"
        echo "  export PATH=\"$INSTALL_DIR:\$PATH\""
        ;;
esac
