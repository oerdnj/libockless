;;; Directory Local Variables
;;; For more information see (info "(emacs) Directory Variables")

((c-mode .
  ((eval .
	 (set (make-local-variable 'directory-of-current-dir-locals-file)
	      (file-name-directory (locate-dominating-file default-directory ".dir-locals.el"))
	      )
	 )
   (eval .
	 (set (make-local-variable 'include-directories)
	      (list

	       ;; top directory
	       (expand-file-name
		(concat directory-of-current-dir-locals-file "./"))

	       (expand-file-name
		(concat directory-of-current-dir-locals-file "include"))

	       (expand-file-name "/usr/local/include")
	       )
	      )
	 )

   (eval setq fill-column 120)
   (eval setq whitespace-line-column 120)
   (eval setq flycheck-clang-include-path include-directories)
   (eval setq flycheck-cppcheck-include-path include-directories)
   (eval setq flycheck-gcc-include-path include-directories)
   (eval setq flycheck-clang-args
	 (list
	  "-include"
	  (expand-file-name
	   (concat directory-of-current-dir-locals-file "config.h"))
	  )
	 )
   (eval setq flycheck-gcc-args
	 (list
	  "-include"
	  (expand-file-name
	   (concat directory-of-current-dir-locals-file "config.h"))
	  )
	 )
   (eval setq flycheck-cppcheck-args
	 (list
	  "--enable=all"
	  "--suppress=missingIncludeSystem"
	  (concat "-include=" (expand-file-name
			       (concat directory-of-current-dir-locals-file "config.h")))
	  )
	 )
   )
  ))
