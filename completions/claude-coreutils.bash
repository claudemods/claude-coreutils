# bash completions for claude-coreutils
if command -v claude-coreutils >/dev/null 2>&1; then
    eval "$(claude-coreutils --completions bash)"
fi
