# Git Quick Guide for This Project

This is a beginner-friendly guide for uploading and updating your project on GitHub.

## 1) One-time setup (first time only)

### Set your Git identity
Use your real name and email once:

```bash
git config --global user.name "Your Name"
git config --global user.email "your_email@example.com"
```

Check:

```bash
git config --get user.name
git config --get user.email
```

### Set remote to your GitHub repo

```bash
git remote set-url origin git@github.com:YOUR_USERNAME/YOUR_REPO.git
git remote -v
```

### SSH auth setup (recommended)

```bash
ssh-keygen -t ed25519 -C "your_email@example.com"
eval "$(ssh-agent -s)"
ssh-add ~/.ssh/id_ed25519
cat ~/.ssh/id_ed25519.pub
```

Copy the printed key and add it in GitHub:
- GitHub Settings
- SSH and GPG keys
- New SSH key

Test:

```bash
ssh -T git@github.com
```

Success message looks like:
"Hi <username>! You've successfully authenticated..."

## 2) Daily workflow (normal use)

From your project folder:

```bash
git status
git add -A
git commit -m "Short clear message"
git push
```

That is the main loop.

## 3) Check you push to the correct place

```bash
git remote -v
git rev-parse --abbrev-ref HEAD
git push --dry-run
```

What to verify:
- Remote URL is your repo
- Current branch is the branch you expect
- Dry-run output shows the right destination

## 4) Fetch vs Pull (simple explanation)

- `git fetch`: download remote changes only
- `git pull`: fetch + merge into your current branch

If you are not sure, do `fetch` first.

## 5) First push of a new branch

```bash
git push -u origin BRANCH_NAME
```

Example:

```bash
git push -u origin riscv
```

After that, normal push is just:

```bash
git push
```

## 6) If GitHub says histories are unrelated

Sometimes `main` and your branch have different roots. Then a GitHub PR may not merge directly.

Local fix:

```bash
git fetch origin
git switch -C main origin/main
git merge YOUR_BRANCH --allow-unrelated-histories
git push -u origin main
```

If conflicts happen:

```bash
git status
# edit files and resolve conflicts
git add -A
git commit
git push
```

## 7) Common errors and quick fixes

### "Author identity unknown"

```bash
git config user.name "Your Name"
git config user.email "your_email@example.com"
```

### "could not read Username for 'https://github.com'"
You are using HTTPS without interactive login.
Use SSH remote instead:

```bash
git remote set-url origin git@github.com:YOUR_USERNAME/YOUR_REPO.git
```

### "Permission denied (publickey)"
Your SSH key is not added to GitHub yet.
Repeat SSH setup and add the public key.

## 8) Recommended habits

- Run `git status` before commit and push
- Use clear commit messages
- Push often (small steps)
- Use `git push --dry-run` if unsure
- Never force-push unless you fully understand it

## 9) Minimal safe command set to remember

```bash
git status
git add -A
git commit -m "message"
git push
```

That is enough for most student projects.
