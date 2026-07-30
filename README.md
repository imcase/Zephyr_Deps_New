# Zephyr Project Dependency Management

This repository contains shared dependencies and deployment scripts designed to work with both **Production** and **Development** workflows.

---

## Tibbotech Workflow (Production)
Designed for a clean, self-contained repository hand-off. This workflow ensures no external Git metadata or submodules are pushed to the Parent Repo.

### 1. Cleanup
Run this if `Zephyr_Deps_New` was previously used as a submodule or if you are resetting the environment.
```bash
# Remove from Git index (if tracked)
git rm -r --cached Zephyr_Deps_New

# Clear internal Git module cache
rm -rf .git/modules/Zephyr_Deps_New

# Physically remove the folder
rm -rf Zephyr_Deps_New
```

### 2. Git Clone
* **NOTE**: run this command in the Parent Root-dir
```bash
git clone git@github.com:imcase/Zephyr_Deps_New.git
```

### 3. Deploy Files
```bash
cd Zephyr_Deps_New
./copy_files_to_parent_repo.sh <folder_name>

# Example:
cd Zephyr_Deps_New
./copy_files_to_parent_repo.sh tibbit_45
```

### 4. Remove Zephyr_Deps_New
```bash
# Move back to Parent Root
cd ..

# Delete the temporary toolbox folder
rm -rf Zephyr_Deps_New
```

### 5. Commit Parent Repo
* **Verification**: Before committing, ensure all target libraries (e.g., lib/tibbit_45) and configuration files (e.g., app.overlay, prj.conf) have been successfully updated in the project root.
```bash
git add .
git commit -m "Added libraries and  configuration files"
```

---

## Imcase Workflow (Development)
Designed for an efficient "Single Source of Truth" environment. This workflow uses Git Submodules and Symlinks to maintain a live connection between the library and the parent project.

### 1. Cleanup
Run this if `Zephyr_Deps_New` was previously used as a submodule or if you are resetting the environment.
```bash
# Remove from Git index (if tracked)
git rm -r --cached Zephyr_Deps_New

# Clear internal Git module cache
rm -rf .git/modules/Zephyr_Deps_New

# Physically remove the folder
rm -rf Zephyr_Deps_New
```

### 2. Git Submodule Add
* **NOTE**: run this command in the Parent Root-dir
```bash
git submodule add git@github.com:imcase/Zephyr_Deps_New.git
```

### 3. Active Linking (Symlinks)
```bash
# Create relative symlinks so that Zephyr finds the library files while they remain inside the submodule.
cd Zephyr_Deps_New
./symlink_files_in_parent_repo.sh <folder_name>

# Example:
cd Zephyr_Deps_New
./symlink_files_in_parent_repo.sh tibbit_45
```

### 4. Commit Parent Repo
* **Verification**: Before committing, ensure all target libraries (e.g., lib/tibbit_45) and configuration files (e.g., app.overlay, prj.conf) have been successfully updated in the project root.
```bash
git add .
git commit -m "Development: Linked Zephyr_Deps_New submodule and created symlinks"
```

### 5. Sync to Remote
* ***Final Step**: Click the SYNC (or Publish) button in VS Code.  
This pushes the Parent Repo's commit to the server so that the submodule link is visible to the rest of the team.
