<p align="center">
  <img src="assets/fast-record-icon.png" width="96" alt="Ícone do Fast Record">
</p>

<h1 align="center">Fast Record</h1>

<p align="center">
  Um gravador de tela para Windows feito para sair do caminho.
</p>

O Fast Record vive na bandeja do sistema e abre um painel compacto quando você precisa dele. A ideia é simples: escolher a fonte, ajustar a qualidade e começar a captura sem atravessar assistentes ou telas desnecessárias.

> [!IMPORTANT]
> O projeto está em desenvolvimento. Captura de tela, janela ou região, áudio em MP4 e os backends NVENC/AMF já estão conectados; webcam ainda está em espera.

## O que já está pronto

- painel flutuante com acesso pela bandeja do Windows;
- controles de gravar, pausar e parar;
- captura do monitor atual em MP4/H.264;
- opção de AV1 em MP4 quando o NVENC da NVIDIA ou o AMF da AMD anuncia suporte ao codec;
- codificação por software ou seleção automática do Media Foundation;
- codificação direta por NVENC quando a GPU NVIDIA compatível está disponível;
- codificação direta por AMD AMF em H.264 ou AV1 quando o driver AMD oferece `amfrt64.dll` (AV1 a partir da RDNA 3, série RX 7000);
- codificação por Intel Quick Sync (QSV) em H.264 quando a GPU Intel e o driver expõem o encoder;
- microfone e áudio do PC mixados em AAC estéreo, inclusive com NVENC e AMF em H.264/AV1;
- captura de tela inteira, janela ou região selecionada;
- histórico local com duração, data, tamanho, reprodução e acesso pelo Explorer;
- resolução configurável de 720p a 4K;
- bitrate ajustável entre 4 e 80 Mbps;
- preferências restauradas depois de fechar e abrir o aplicativo;
- opção para iniciar automaticamente com o Windows, já minimizado na bandeja;
- detecção em tempo de execução dos drivers NVENC e AMF;
- instância única: abrir o executável novamente traz o painel existente para frente.

As preferências ficam em **HKEY_CURRENT_USER\Software\Fast Record**. Nenhuma configuração é enviada para fora do computador.

## Interface

A janela é nativa em C++, com a camada visual renderizada pelo WebView2. Isso mantém a integração com o Windows — bandeja e captura — sem limitar a interface aos controles clássicos do Win32.

## Por baixo do painel

    TrayIcon + comandos
            │
            ▼
     AppController ───── AppSettings
            │
            ▼
    RecorderController
        ┌────┼─────────────┬──────────────┐
        ▼    ▼             ▼              ▼
     NVENC  AMD AMF     Intel QSV     Media Foundation
   H.264/AV1 H.264/AV1   H.264         H.264/software
        └──────────── WASAPI + AAC ────────────┘

A captura usa Windows Graphics Capture, com redimensionamento pelo Direct3D 11. A proporção do monitor é preservada: resoluções diferentes recebem margens pretas quando necessário. O cursor é capturado pelo Windows.

No modo software/automático, os frames são copiados para memória antes da codificação H.264 pelo Media Foundation. No modo NVENC, a textura D3D11 da captura vai direto para o encoder da NVIDIA e o pacote H.264 ou AV1 é muxado em MP4 localmente. No modo AMF, o próprio processador de vídeo da GPU redimensiona e converte o frame para NV12 (BT.709, faixa limitada), o AMF copia essa textura e a codifica em H.264 ou AV1 sem passar pela CPU, e o pacote é muxado em MP4 pelo mesmo escritor usado no NVENC. No modo Intel QSV, o Media Foundation recebe um dispositivo D3D11 associado à GPU Intel e procura somente o caminho de hardware compatível com o Quick Sync. Quando o microfone ou o áudio do PC está ativo, o WASAPI captura as fontes selecionadas, faz a mistura e adiciona uma faixa AAC estéreo de 48 kHz ao mesmo arquivo. AV1 está disponível pelo **NVENC** e pelo **AMF**; o QSV começa com H.264 para manter uma combinação compatível com mais gerações de GPU.

Se o monitor for desconectado, sua resolução mudar ou a GPU falhar, o app tenta finalizar o MP4 e informa o erro. É necessário iniciar uma nova gravação após a mudança. Conteúdo protegido pode não aparecer; HDR ainda não tem tratamento de cor dedicado.

## Compilando

### Requisitos

- Windows 10 versão 1903 ou posterior, ou Windows 11;
- Visual Studio com o workload **Desktop development with C++**;
- Windows SDK;
- CMake 3.25 ou mais recente;
- Microsoft Edge WebView2 Runtime.

Os binários do Fast Record usam o runtime C++ estático no Windows. Isso evita
que uma versão antiga do Visual C++ instalada na máquina interfira na captura;
o instalador não precisa baixar ou substituir o runtime MSVC.

### Build básica

    cmake -S . -B build -A x64
    cmake --build build --config Release

O executável será criado em:

    build/Release/fast-record.exe

O SDK do WebView2 é obtido pelo CMake durante a configuração.

### Teste local da captura

O teste captura o monitor principal duas vezes, com pausa e retomada, em modo software e automático. Execute em uma sessão desktop interativa. Os vídeos permanecem na pasta indicada para inspeção:

    cmake --build build --config Debug --target capture-smoke
    build\Debug\capture-smoke.exe build\capture-smoke-output

Esse teste não roda no CI nem faz parte da build padrão.

### Testes automatizados

Depois de configurar e compilar o projeto, rode a suíte determinística com:

    ctest --test-dir build -C Release --output-on-failure

Ela verifica as preferências salvas no Registro, os estados e erros do
gravador, o contrato da interface, a comparação de versões e os metadados do
atualizador, as regras do áudio, as sondas de NVENC, AMF e Intel QSV e o
muxer MP4 (cabeçalho AV1 e marcação de key frames). Os testes `capture-smoke`,
`nvenc-smoke` e `amf-smoke` continuam separados porque precisam de uma sessão
gráfica, dispositivos de áudio e, no caso do NVENC e do AMF, uma GPU da NVIDIA
ou da AMD compatível.

## Releases

Todo push na branch `main` dispara o workflow de release. A versão é criada a
partir do número da execução, seguindo o formato `v0.1.<build>`; assim, cada
commit publicado recebe uma tag própria e não sobrescreve uma versão anterior.

Cada release publicado no GitHub contém:

- `FastRecord-0.1.<build>-x64-Setup.exe`, instalador por usuário;
- `FastRecord-0.1.<build>-x64.zip`, versão portátil.

O instalador cria atalhos no Menu Iniciar e na área de trabalho, registra o
desinstalador e não precisa de privilégios de administrador. O Microsoft Edge
WebView2 Runtime continua sendo um requisito do Windows.

Nas instalações feitas pelo Setup, o app consulta novas versões ao abrir. A
aba **Configurações** também permite verificar manualmente e iniciar a
atualização. Antes de abrir o Setup, o app valida o tamanho e o SHA-256 do
arquivo publicado. A versão portátil continua sendo atualizada manualmente
com o ZIP da release.

### NVENC

O CMake busca automaticamente uma versão fixada dos headers do NVENC quando o SDK não está instalado. Para usar um SDK local:

    cmake -S . -B build -A x64 -DFASTRECORD_NVENC_SDK_DIR="C:\SDKs\Video_Codec_SDK"
    cmake --build build --config Release

O teste direto captura três segundos usando a GPU NVIDIA e gera um MP4 em `build/nvenc-smoke-output`:

    cmake --build build --config Debug --target nvenc-smoke
    build\Debug\nvenc-smoke.exe build\nvenc-smoke-output

Para exercitar o caminho AV1, passe `av1` como segundo argumento. O teste só
funciona em uma GPU NVIDIA cujo driver anuncia AV1:

    build\Debug\nvenc-smoke.exe build\nvenc-smoke-output-av1 av1

### AMD AMF

    cmake -S . -B build -A x64 -DFASTRECORD_AMF_SDK_DIR="C:\SDKs\AMF"

Os headers oficiais do AMF são baixados e fixados automaticamente quando o SDK
local não existe. O runtime não é empacotado: o app procura
`amfrt64.dll`/`amfrtlt64.dll` instalado pelo driver AMD. Sem uma GPU ou driver
AMD, a sonda informa a ausência da DLL e a seleção AMF falha com uma mensagem
clara; os modos automático, software e NVENC continuam independentes.

Para testar só o carregamento e o diagnóstico do runtime:

    build\Release\fast-record-tests.exe

O teste não finge uma GPU AMD. Em uma máquina sem AMD ele valida justamente o
caminho de ausência da DLL; a codificação AMF efetiva é exercitada pelo
`amf-smoke`, que grava pela GPU AMD e depois decodifica o MP4 inteiro com o
Media Foundation, conferindo codec, resolução, duração e frames entregues:

    cmake --build build --config Release --target amf-smoke
    build\Release\amf-smoke.exe build\amf-smoke-output --codec=av1 --size=1920x1080 --fps=60 --animate

`--codec=h264|av1`, `--seconds=N`, `--size=WxH`, `--fps=N`, `--bitrate=N`,
`--pause=N`, `--mic` e `--no-system-audio` ajustam o cenário. `--sound-at=N`
toca um som do Windows N segundos depois do início e confere se ele aparece
nesse ponto do arquivo, e não no começo da faixa. `--animate` abre
uma pequena janela que muda de cor a cada frame, para medir a vazão com a tela
sempre mudando. Validar o AV1 exige a extensão **AV1 Video Extension** da
Microsoft Store, usada pelo Media Foundation para decodificar.

O AV1 pelo AMF exige uma GPU RDNA 3 ou mais recente. O app pede ao driver o
modo de alinhamento sem restrições; GPUs que só codificam AV1 em blocos de
64×16 (como a RDNA 3) recusam 1080p com uma mensagem sugerindo 720p, 1440p ou
4K, que são múltiplos desse bloco. Validado em uma RX 9070 XT (RDNA 4), que
aceita qualquer resolução.

### Intel Quick Sync

O Fast Record procura os encoders de vídeo Intel registrados pelo driver via
Media Foundation e enumera a GPU Intel pelo Direct3D 11. A opção **QSV · Intel**
fica disponível no painel e exige uma GPU Intel física e um driver compatível;
quando o hardware não existe, a gravação falha com um diagnóstico em vez de
usar software silenciosamente.

O teste unitário exercita a enumeração e o caminho de ausência. Para validar a
gravação efetiva, selecione **QSV · Intel** em um notebook com Iris ativa no
Gerenciador de Dispositivos, use H.264 e faça uma captura curta.

## Estrutura

    assets/          ícone e recursos visuais
    src/app/         ciclo de vida, janela e preferências
    src/platform/    integração com a bandeja do Windows
    src/recording/   estado do gravador e sondas NVENC/AMF/QSV
    src/resources/   recursos compilados no executável
    src/ui/          WebView2 e interface do painel

## Próximos passos

- [x] gravar o monitor atual em MP4/H.264;
- [x] migrar a captura para Windows Graphics Capture e Direct3D 11;
- [x] capturar e mixar microfone e áudio do sistema com WASAPI;
- [x] conectar NVENC ao fluxo de frames;
- [x] adicionar escolha de codec H.264/AV1 ao NVENC;
- [x] conectar AMD AMF H.264 ao fluxo de frames;
- [x] conectar Intel Quick Sync H.264 via Media Foundation;
- [x] adicionar AV1 ao backend AMD AMF;
- [x] implementar captura de janela e região;
- [x] listar e reproduzir gravações recentes;
- [x] empacotar versões automaticamente pelo GitHub Actions.

## Licença

O Fast Record é distribuído sob a licença [MIT](LICENSE). O texto completo
está no arquivo `LICENSE` na raiz do repositório.
