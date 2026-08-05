REPORT := technical_report
OUTDIR := ../build/docs
DELIVERY_DIR := ../output/pdf

.PHONY: all clean

all:
	mkdir -p $(OUTDIR)
	latexmk -xelatex -interaction=nonstopmode -halt-on-error \
		-outdir=$(OUTDIR) $(REPORT).tex
	mkdir -p $(DELIVERY_DIR)
	cp $(OUTDIR)/$(REPORT).pdf $(DELIVERY_DIR)/$(REPORT).pdf

clean:
	latexmk -C -outdir=$(OUTDIR) $(REPORT).tex
