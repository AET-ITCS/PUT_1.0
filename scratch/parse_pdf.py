import re
import os
import sys

# Force output encoding to UTF-8 to prevent GBK UnicodeEncodeError on Windows
if sys.stdout.encoding != 'utf-8':
    try:
        import io
        sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
    except Exception:
        pass

def parse_pdf_binary(pdf_path):
    print(f"Trying binary scanning for: {pdf_path}")
    if not os.path.exists(pdf_path):
        print(f"File not found: {pdf_path}")
        return
        
    with open(pdf_path, 'rb') as f:
        content = f.read()
        
    patterns = [
        re.compile(b'(?i)rs485[A-Za-z0-9_]*'),
        re.compile(b'(?i)uart[A-Za-z0-9_]*'),
        re.compile(b'(?i)ttys[0-9]*'),
        re.compile(b'(?i)sp3485[A-Za-z0-9_]*'),
        re.compile(b'(?i)max3485[A-Za-z0-9_]*'),
        re.compile(b'(?i)gpi[a-z0-9_]*'),
    ]
    
    found = set()
    for pattern in patterns:
        matches = pattern.findall(content)
        for m in matches:
            try:
                found.add(m.decode('utf-8', errors='ignore'))
            except Exception:
                pass
                
    print(f"Found {len(found)} raw matches in binary:")
    for f in sorted(list(found))[:50]: # Limit to 50
        print(" -", f)

def parse_pdf_with_libs(pdf_path):
    if not os.path.exists(pdf_path):
        print(f"File not found: {pdf_path}")
        return None

    # Try pdfminer (we saw pdfminer.six is in the pip list!)
    try:
        from pdfminer.high_level import extract_text
        print("Using pdfminer...")
        text = extract_text(pdf_path)
        return text
    except Exception as e:
        print(f"pdfminer error: {e}")

    # Try PyPDF2
    try:
        import PyPDF2
        print("Using PyPDF2...")
        reader = PyPDF2.PdfReader(pdf_path)
        text = ""
        for page in reader.pages:
            text += page.extract_text() or ""
        return text
    except Exception as e:
        print(f"PyPDF2 error: {e}")
        
    return None

def main():
    pdf1 = os.path.join("d:/", "xdev", "sourcecode", "PUT", "PUT_1.0", "docs", "原理图", "duo_iob_v1.11.pdf")
    pdf2 = os.path.join("d:/", "xdev", "sourcecode", "PUT", "PUT_1.0", "docs", "手册", "sg2002_trm_cn.pdf")
    
    for pdf_path in [pdf1]: # Only focus on IOB first
        print(f"\n=== Analyzing {os.path.basename(pdf_path)} ===")
        text = parse_pdf_with_libs(pdf_path)
        if text:
            # Search for RS485 and UART occurrences
            rs485_matches = re.findall(r'(?i)[a-z0-9_]*rs485[a-z0-9_]*', text)
            uart_matches = re.findall(r'(?i)[a-z0-9_]*uart[a-z0-9_]*', text)
            gpio_matches = re.findall(r'(?i)[a-z0-9_]*gpio[a-z0-9_]*', text)
            sp_matches = re.findall(r'(?i)[a-z0-9_]*sp3485[a-z0-9_]*', text)
            print("RS485 matches in text:", set(rs485_matches))
            print("UART matches in text:", set(uart_matches))
            print("GPIO matches in text:", set(gpio_matches))
            print("SP3485 matches in text:", set(sp_matches))
            # Print lines containing RS485 or UART
            lines = text.split('\n')
            print("Relevant lines (first 50 matches):")
            count = 0
            for line in lines:
                lower_line = line.lower()
                if any(x in lower_line for x in ['rs485', 'uart', 'sp3485', 'tty', 'de/re', 're/de', 'rts', 'cts', 'tx', 'rx']):
                    print("  ", line.strip())
                    count += 1
                    if count >= 50:
                        break
        else:
            parse_pdf_binary(pdf_path)
        
if __name__ == "__main__":
    main()


