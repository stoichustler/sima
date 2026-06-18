# VIM


## Compilation

- VIM source code:

```sh
git clone https://github.com/vim/vim.git ~/opt/vim-src
```

- Packages needed:

```sh
sudo apt install -y libncurses-dev libx11-dev libxt-dev libsm-dev libice-dev libxpm-dev
```

- Compilation

```sh
./configure --with-features=huge \
            --enable-multibyte \
            --enable-python3interp=yes \
            --with-python3-config-dir=$(python3-config --configdir) \
            --enable-perlinterp=yes \
            --enable-luainterp=yes \
            --enable-cscope \
            --enable-gui=no \
            --with-x \
            --prefix=$HOME/opt

make -j32

make install
```

- Ready to Use

```sh
export PATH=$HOME/opt/bin:$PATH
```

```sh
git clone https://github.com/preservim/nerdtree.git ~/.vim/pack/plugins/start/nerdtree
git clone https://github.com/jiangmiao/auto-pairs.git ~/.vim/pack/plugins/start/auto-pairs
git clone https://github.com/preservim/nerdcommenter.git ~/.vim/pack/plugins/start/nerdcommenter
git clone https://github.com/tpope/vim-surround.git ~/.vim/pack/plugins/start/vim-surround
git clone https://github.com/vim-airline/vim-airline.git ~/.vim/pack/plugins/start/vim-airline
git clone https://github.com/catppuccin/vim ~/.vim/pack/themes/start/catppuccin
git clone --depth 1 https://github.com/junegunn/fzf.git ~/.fzf
~/.fzf/install
git clone https://github.com/junegunn/fzf.vim.git ~/.vim/pack/plugins/start/fzf
```

- Copy VIM [.vimrc](.vimrc) to `$HOME`.


---

Hustle Embedded OS.
