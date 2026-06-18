" -------------------------------------------
set autoread
set magic
set number
set relativenumber
" -------------------------------------------
set mouse=a
" -------------------------------------------
set nocompatible
set scrolloff=10
" -------------------------------------------
colorscheme catppuccin_mocha
set termguicolors
set background=dark
" -------------------------------------------
set encoding=utf-8
set termencoding=utf-8
" -------------------------------------------
set autoindent
set cindent
set smarttab

" <<Default Indentation>>
set tabstop=4
set shiftwidth=4
set softtabstop=4
set expandtab

" Default C/C++
command! HFmt setlocal
  \ tabstop=4
  \ shiftwidth=4
  \ softtabstop=4
  \ expandtab

" Google C/C++
command! GFmt setlocal
  \ tabstop=2
  \ shiftwidth=2
  \ softtabstop=2
  \ expandtab

" Linux C/C++
command! LFmt setlocal
  \ tabstop=4
  \ shiftwidth=4
  \ softtabstop=4
  \ noexpandtab

" -------------------------------------------
set clipboard=unnamedplus
nnoremap <silent> <C-c> "+y
vnoremap <silent> <C-c> "+y
" -------------------------------------------
set confirm
set linespace=0
" -------------------------------------------
" set cursorline
set list
set listchars=tab:·\ ,precedes:←,extends:→
" -------------------------------------------
set wrap
set linebreak
set breakindent
set breakindentopt=shift:2
" set showbreak=➢
" -------------------------------------------
set laststatus=2
set hlsearch
set incsearch
set matchtime=1
set nobackup
set noswapfile
set guioptions-=T
set guioptions-=m
set report=0
set paste
set cursorline
" -------------------------------------------
" set autochdir
set wildmenu
set wildmode=full
set wildoptions=pum
set completeopt=menu,preview,longest
set confirm
set previewpopup=height:20,width:80
" vertical
set fillchars=vert:\ ,
set iskeyword+=_,$,@,%,#,-
" set ruler
" -------------------------------------------
" Enable key<del>
set backspace=2
syntax on
set tags=tags;
" -------------------------------------------
" statusline
set statusline=
set statusline+=%{'BEAU'}\ %F
set statusline+=%h%m%r%w
set statusline+=%=%y\ %(%l:%c%V%)
set statusline+=\ [%p%%]

set rtp+=~/.fzf

" Type `space`
let mapleader=' '
let timeoutlen=120

" -------------------------------------------
" NERDTree configuration
let g:NERDTreeWinSize = 35
let g:NERDTreeWinSizeMax = 40
let g:NERDTreeShowHidden = 1
let g:NERDTreeAutoCenter = 1
let g:NERDTreeChDirMode = 2
let g:NERDTreeAutoRefresh = 1
" -------------------------------------------
" netrw configuration
let g:netrw_winsize   = 20
let g:netrw_liststyle = 3
let g:netrw_hide      = 1
let g:netrw_banner    = 0
let g:netrw_keepdir   = 1
let g:netrw_sort_by   = "name"
" -------------------------------------------
let g:lightline = { 'colorscheme': 'catppuccin_mocha' }
" -------------------------------------------
" Need X11 support 
vnoremap <silent> <C-c> "+y
nnoremap <silent> <C-p> "+p
" -------------------------------------------
nnoremap <silent> <Leader>e :NERDTreeToggle<CR>
nnoremap <silent> <Leader>f :FZF<CR>
" -------------------------------------------
nnoremap <silent> <Leader>t :tab terminal<CR>
nnoremap <silent> <Leader>w <C-w><C-w>
" -------------------------------------------
" This for ctags: g-]    Display all possible definitions
"                 ctrl-] Jump to first matched definition
nnoremap <silent> <Leader>g g]
nnoremap <silent> <Leader>b <C-t>
" -------------------------------------------
" Jump up/down efficiently
nnoremap <silent> <Leader>d }
nnoremap <silent> <Leader>u {
" -------------------------------------------
" Switch to system clipboard (clean clipboard)
" Add `:GitGutterToggle<CR>` if vim-gitgutter plugin installed
nnoremap <silent> <Leader>n
    \ :set nonu<CR>:set mouse=<CR>:set norelativenumber<CR>
    \ :set nolist<CR>
nnoremap <silent> <Leader>m
    \ :set nu<CR>:set mouse=a<CR>:set relativenumber<CR>
    \ :set list<CR>
" -------------------------------------------
" Autocomplete (Ctrl-n/p)
" -------------------------------------------
" Jump to last/next function [[ and ]]
nnoremap <silent> <Leader>x <C-[>
" -------------------------------------------
" End of settings

