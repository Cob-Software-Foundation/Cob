# kernels -- text, kernel by kernel
#   kernel_len  kernel_upper  kernel_lower  kernel_slice  kernel_find
#   kernel_repeat  kernel_trim  kernel_replace  kernel_char  kernel_code
#   kernel_from_code  kernel_to_int  kernel_starts  kernel_ends
extern kernel_len(s) from "kernels"
extern kernel_upper(s) from "kernels"
extern kernel_lower(s) from "kernels"
extern kernel_slice(s, start, count) from "kernels"
extern kernel_find(hay, needle) from "kernels"
extern kernel_repeat(s, n) from "kernels"
extern kernel_trim(s) from "kernels"
extern kernel_replace(s, from_text, to_text) from "kernels"
extern kernel_char(s, i) from "kernels"
extern kernel_code(s) from "kernels"
extern kernel_from_code(n) from "kernels"
extern kernel_to_int(s) from "kernels"
extern kernel_starts(s, prefix) from "kernels"
extern kernel_ends(s, suffix) from "kernels"
