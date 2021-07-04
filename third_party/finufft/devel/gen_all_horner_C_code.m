% Script to make all C code for looped Horner eval of kernels of all widths.
% writes to "ker" array, from a variable "z", and switches by width "w".
% Resulting C code needs only placing in a function.

% Barnett 4/23/18; now calling Ludvig's loop version from 4/25/18.
% version including low upsampfac, 6/17/18.
% Ludvig put in w=4n padding, 1/31/20. Mystery about why d was bigger 2/6/20.
clear
opts = struct();

ws = 2:16;
upsampfac = 2;       % sigma (upsampling): either 2 (default) or low (eg 5/4).
opts.wpad = true;    % pad kernel eval to multiple of 4

if upsampfac==2, fid = fopen('../src/ker_horner_allw_loop.c','w');
else, fid = fopen('../src/ker_lowupsampfac_horner_allw_loop.c','w');
end
fwrite(fid,sprintf('// Code generated by gen_all_horner_C_code.m in finufft/devel\n'));
fwrite(fid,sprintf('// Authors: Alex Barnett & Ludvig af Klinteberg.\n// (C) The Simons Foundation, Inc.\n'));
for j=1:numel(ws)
  w = ws(j)
  if upsampfac==2    % hardwire the betas for this default case
    betaoverws = [2.20 2.26 2.38 2.30];   % matches setup_spreader
    beta = betaoverws(min(4,w-1)) * w;    % uses last entry for w>=5
    d = w + 2 + (w<=8);                   % between 2-3 more degree than w
  else               % use formulae, must match params in setup_spreader...
    gamma=0.97;                           % safety factor
    betaoverws = gamma*pi*(1-1/(2*upsampfac));  % from cutoff freq formula
    beta = betaoverws * w;
    d = w + 1 + (w<=8);                   % less, since beta smaller, smoother
  end
  str = gen_ker_horner_loop_C_code(w,d,beta,opts);
  if j==1                                % write switch statement
    fwrite(fid,sprintf('  if (w==%d) {\n',w));
  else
    fwrite(fid,sprintf('  } else if (w==%d) {\n',w));
  end
  for i=1:numel(str); fwrite(fid,['    ',str{i}]); end
end
fwrite(fid,sprintf('  } else\n    printf("width not implemented!\\n");\n'));
fclose(fid);
